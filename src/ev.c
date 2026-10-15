#include "ev.h"
#include "log.h"
#include <assert.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>

#define IOU_SIZE_WANT 128 // iou requested ring size from kernel

static int ev__setup_io_uring(ev_loop_t *loop) {
  struct io_uring_params params;
  struct ev__iou_ring sqe;
  struct ev__iou_ring cqe;
  struct io_uring_cqe *cqes;
  struct io_uring_sqe *sqes;
  void *sq, *cq;
  int32_t ringfd;

  memset(&params, 0, sizeof params);
  params.flags = IORING_SETUP_SQPOLL | IORING_SETUP_NO_SQARRAY;
  ringfd = io_uring_setup(IOU_SIZE_WANT, &params);
  if (ringfd < 1) {
    log_error("io_uring_setup: %s", strerror(-ringfd));
    exit(1);
  }

  sq = mmap(NULL, params.sq_off.array + params.sq_entries * sizeof(uint32_t),
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ringfd,
            IORING_OFF_SQ_RING);
  cq =
      mmap(NULL,
           params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe),
           PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ringfd,
           IORING_OFF_CQ_RING);

  sqes = mmap(NULL, params.sq_entries * sizeof(struct io_uring_sqe),
              PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ringfd,
              IORING_OFF_SQES);

  if (sq == MAP_FAILED || cq == MAP_FAILED || sqes == MAP_FAILED)
    goto fail;

  sqe.head = sq + params.sq_off.head;
  sqe.tail = sq + params.sq_off.tail;
  sqe.mask = sq + params.sq_off.ring_mask;
  sqe.flags = sq + params.sq_off.flags;

  cqe.head = cq + params.cq_off.head;
  cqe.tail = cq + params.cq_off.tail;
  cqe.mask = cq + params.cq_off.ring_mask;
  cqe.flags = cq + params.cq_off.flags;
  cqes = cq + params.cq_off.cqes;

  loop->iou_sqe = sqe;
  loop->iou_cqe = cqe;
  loop->sqes = sqes;
  loop->cqes = cqes;
  loop->ringfd = ringfd;

  log_debug("initialized io_uring: sq_entries=%d sqe_mask=0x%x08 "
            "cqe_entries=%d cqe_mask=0x%x08",
            params.sq_entries, *sqe.mask, params.cq_entries, *cqe.mask);

  return 0;

fail:
  perror("mmap");
  exit(1);
}

static int ev__iou_submit(ev_loop_t *loop) {
  struct ev__iou_ring *iou;
  uint32_t flags, ret;

  iou = &loop->iou_sqe;
  // By this point, the SQE should already be filled in
  // and we only need to increment tail for kernel to consume it
  atomic_store_explicit((_Atomic uint32_t *)iou->tail, *iou->tail + 1,
                        memory_order_release);

  // check flags to see if kernel requested a wake up
  flags = atomic_load_explicit((_Atomic uint32_t *)iou->flags,
                               memory_order_acquire);

  if (flags & IORING_SQ_NEED_WAKEUP) {
    if ((ret = io_uring_enter(loop->ringfd, 1, 0, IORING_ENTER_SQ_WAKEUP,
                              NULL)) < 0) {
      printf("error: %s\n", strerror(-ret));
      exit(1);
    }
  }

  return 0;
}

/// req's lifetime must be valid until reap time
int ev_iou_req(ev_loop_t *loop, ev_io_req_t *req) {
  struct io_uring_sqe *sqe;
  uint32_t head, tail, index;

  // sqe head owned by kernel
  head = atomic_load_explicit((_Atomic uint32_t *)loop->iou_sqe.head,
                              memory_order_acquire);
  tail = *loop->iou_sqe.tail;
  index = tail & *loop->iou_sqe.mask;

  if (tail - head > *loop->iou_sqe.mask) {
    return EV_FULL;
  }

  sqe = &loop->sqes[index];
  memset(sqe, 0, sizeof *sqe);
  sqe->fd = req->fd;
  sqe->opcode = req->opcode;
  sqe->off = req->off;
  sqe->len = req->len;
  sqe->addr = req->addr;
  sqe->poll32_events = req->flags;
  sqe->user_data = (uintptr_t)req;
  sqe->flags = 0;

  log_debug(
      "submitting SQE: index=%d mask=0x%x08 raw_index(t)=%d opcode=%d fd=%d, "
      "off=%ld len=%ld addr=%ld, "
      "poll_events=%d user_data=%ld",
      index, *loop->iou_sqe.mask, tail, sqe->opcode, sqe->fd, sqe->off,
      sqe->len, sqe->addr, sqe->poll32_events, sqe->user_data);

  return ev__iou_submit(loop);
}

static int ev__io_poll(ev_loop_t *loop) {
  return io_uring_enter(loop->ringfd, 0, 1, IORING_ENTER_GETEVENTS, NULL);
}

int ev_loop_init(ev_loop_t *loop) {
  ev__setup_io_uring(loop);
  return 0;
}

int ev_loop_run(ev_loop_t *loop) {
  uint32_t i, head, tail, mask, index, ret, n;
  ev_io_req_t *req;

  while (1) {

    if ((ret = ev__io_poll(loop)) < 0) {
      log_error("ev_loop_run: poll error: %s", strerror(-ret));
      exit(1);
    }

    tail = atomic_load_explicit((_Atomic uint32_t *)loop->iou_cqe.tail,
                                memory_order_acquire);
    head = *loop->iou_cqe.head;
    mask = *loop->iou_cqe.mask;
    n = 0;
    for (i = head; i != tail; i++) {
      index = i & mask;

      log_trace("reaping CQE index=%d mask=0x%x08 raw_index(h)=%d res=%d "
                "user_data=%ld",
                index, mask, i, loop->cqes[index].res,
                loop->cqes[index].user_data);

      req = (ev_io_req_t *)loop->cqes[index].user_data;
      log_trace("  > opcode=%d fd=%d res=%d", req->opcode, req->fd,
                req->result);
      req->result = loop->cqes[index].res;
      req->result_flags = loop->cqes[index].flags;
      req->callback(req);
      n++;
    }

    log_trace("ev_loop_run: reaped %d CQEs", n);
    atomic_store_explicit((_Atomic uint32_t *)loop->iou_cqe.head, tail,
                          memory_order_release);
  }

  return 0;
}
