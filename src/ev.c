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

static const char *opname(int opcode) {
  switch (opcode) {
  case IORING_OP_POLL_ADD:
    return "POLL";
    break;
  case IORING_OP_OPENAT:
    return "OPENAT";
    break;
  case IORING_OP_READ:
    return "READ";
    break;
  default:
    return "UNKNOWN";
    break;
  }
}

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
    LOG_ERROR("io_uring_setup: %s", strerror(-ringfd));
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
  loop->pending = 0;

  LOG_DEBUG("initialized io_uring: sq_entries=%d sqe_mask=0x%x08 "
            "cqe_entries=%d cqe_mask=0x%x08",
            params.sq_entries, *sqe.mask, params.cq_entries, *cqe.mask);

  return 0;

fail:
  perror("mmap");
  exit(1);
}

static int ev__iou_submit(ev_loop_t *loop) {
  struct ev__iou_ring *iou;
  uint32_t flags, pending;
  int ret;

  pending = loop->pending;
  if (!pending)
    return 0;

  iou = &loop->iou_sqe;
  // By this point, the SQEs should already be filled in
  // and we only need to increment tail for kernel to consume them
  atomic_store_explicit((_Atomic uint32_t *)iou->tail,
                        *iou->tail + loop->pending, memory_order_release);

  loop->pending = 0;

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

  LOG_DEBUG("submitted %d SQEs", pending);

  return pending;
}

/// req's lifetime must be valid until reap time
int ev_iou_req(ev_loop_t *loop, ev_io_req_t *req) {
  struct io_uring_sqe *sqe;
  uint32_t head, tail, index;

  // sqe head owned by kernel
  head = atomic_load_explicit((_Atomic uint32_t *)loop->iou_sqe.head,
                              memory_order_acquire);
  tail = *loop->iou_sqe.tail;
  index = (tail & *loop->iou_sqe.mask) + loop->pending;

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

  loop->pending += 1;

  LOG_DEBUG("scheduling SQE index=%d mask=0x%x08 raw_index(t)=%d", index,
            *loop->iou_sqe.mask, tail);

  LOG_DEBUG("scheduled SQE req=%p opname=%s opcode=%d fd=%d, off=%ld len=%ld "
            "addr=%ld, poll_events=%d user_data=%ld",
            req, opname(sqe->opcode), sqe->opcode, sqe->fd, sqe->off, sqe->len,
            sqe->addr, sqe->poll32_events, sqe->user_data);

  // TODO: limit to SQPOLL rings so kernel thread can start doing work
  // immediately during callback handling, otherwise, only send batched
  // submissions
  ev__iou_submit(loop);
  return 0;
}

static int ev__io_poll(ev_loop_t *loop) {
  LOG_DEBUG("ev_loop poll");
  return io_uring_enter(loop->ringfd, 0, 1, IORING_ENTER_GETEVENTS, NULL);
}

int ev_loop_init(ev_loop_t *loop) {
  ev__setup_io_uring(loop);
  return 0;
}

int ev_loop_run(ev_loop_t *loop) {
  uint32_t i, head, tail, mask, index, n;
  int ret, callback_ret;
  ev_io_req_t *req;

  LOG_DEBUG("starting ev_loop");

  ev__iou_submit(loop); // send any initial pending SQEs
  while (1) {

    if ((ret = ev__io_poll(loop)) < 0) {
      LOG_ERROR("ev_loop_run: poll error: %s", strerror(-ret));
      exit(1);
    }

    tail = atomic_load_explicit((_Atomic uint32_t *)loop->iou_cqe.tail,
                                memory_order_acquire);
    head = *loop->iou_cqe.head;
    mask = *loop->iou_cqe.mask;
    n = 0;
    callback_ret = 0;
    for (i = head; i != tail; i++) {
      index = i & mask;

      LOG_DEBUG("reaping CQE index=%d mask=0x%x08 raw_index(h)=%d res=%d "
                "user_data=%ld",
                index, mask, i, loop->cqes[index].user_data);

      req = (ev_io_req_t *)loop->cqes[index].user_data;
      req->result = loop->cqes[index].res;
      req->result_flags = loop->cqes[index].flags;

      LOG_DEBUG(
          "reaping CQE req=%p opname=%s opcode=%d fd=%d res=%d user_data=%ld",
          req, opname(req->opcode), req->opcode, req->fd, req->result,
          loop->cqes[index].user_data);
      if ((callback_ret = req->callback(req)) == EV_EXIT) {
        LOG_DEBUG("ev_loop: normal exit requested");
        goto release;
      }
      n++;
    }

  release:
    LOG_TRACE("ev_loop_run: reaped %d CQEs", n);
    atomic_store_explicit((_Atomic uint32_t *)loop->iou_cqe.head, tail,
                          memory_order_release);
    if (callback_ret == EV_EXIT) {
      LOG_DEBUG("ev_loop exiting");
      break;
    }

    // submit any pending requests collected during callback handling
    ev__iou_submit(loop);
  }

  return 0;
}
