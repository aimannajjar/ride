#include "processor.h"
#include "ev.h"
#include "hasher.h"
#include "log.h"
#include "queue.h"
#include "ride.h"
#include "verifier.h"
#include <assert.h>
#include <emmintrin.h>
#include <fcntl.h>
#include <jemalloc/jemalloc.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <limits.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

// need 4K-aligned buffer for O_DIRECT reads
static_assert(!(READ_BUF_SIZE & (4096 - 1)), "BUFFER SIZE must be 4K aligned");

// container for ev_io_req and hasher
struct processor_iou_req {
  ev_io_req_t req;
  struct hasher hasher;
  struct processor_iou_req *next;
  unsigned char *buffer;
  void *data;
  void *data2;
  uint16_t in_flight;
  uint32_t id;
};

static void processor_prepare_poll_reqs(struct processor *processor);

static void processor_submit_poll_reqs(struct processor *processor) {
  ev_iou_req(&processor->loop, &processor->queue_poll_req);
}

static void processor_setup(struct processor *processor,
                            const struct processor_args *wargs) {

  ssize_t i;
  processor->id = wargs->id;
  processor->nr_tasks = wargs->io_concurrency;
  processor->queue_efd = wargs->queue_eventfd;
  processor->buffers =
      aligned_alloc(4096, processor->nr_tasks * sizeof(*processor->buffers));
  processor->reqs = mallocx(
      processor->nr_tasks * sizeof(struct processor_iou_req), MALLOCX_ZERO);
  processor->reqs_list = processor->reqs;
  processor->nr_free_reqs = processor->nr_tasks;

  if (!processor->buffers || !processor->reqs) {
    perror("malloc");
    exit(1);
  }

  // assign a buffer to each req and setup linked list
  for (i = 0; i < processor->nr_tasks; i++) {
    processor->reqs[i].id = i;
    processor->reqs[i].req.data2 = &processor->reqs[i];
    processor->reqs[i].buffer = processor->buffers[i];
    processor->reqs[i].next = &processor->reqs[i + 1];
  }
  processor->reqs[processor->nr_tasks - 1].next = NULL;

  log_debug("setup processor: queue_fd=%d", processor->queue_efd);
  ev_loop_init(&processor->loop);
  processor_prepare_poll_reqs(processor);
  processor_submit_poll_reqs(processor);
}

[[maybe_unused]]
static void processor_free(struct processor *processor) {
  free(processor->buffers);
  fflush(stdout);
  fflush(stderr);
}

ev_io_req_t *processor_get_req(struct processor *processor) {
  ev_io_req_t *req;
  struct processor_iou_req *preq; // preq is a container for req

  if (processor->reqs_list == NULL) {
    log_error("processor requests queue full");
    // TODO: handle full queue
    exit(1);
  }

  preq = processor->reqs_list;
  processor->reqs_list = processor->reqs_list->next;

  assert(!preq->in_flight && "req slot is already inflight");
  preq->in_flight = 1;

  // get a req slot and reset it, keep data2 becuase it points to the container
  // which includes pre-allocated buffers
  req = &preq->req;
  req->flags = 0;
  req->result = 0;
  req->data = processor;
  req->data2 = preq;
  req->off = 0;
  req->callback = NULL;
  req->opcode = 0;
  req->addr = 0;
  req->len = 0;

  processor->nr_free_reqs--;
  return req;
}

void processor_free_req(ev_io_req_t *req) {
  struct processor *processor;
  struct processor_iou_req *preq;
  processor = req->data;
  preq = req->data2;

  log_trace("processor_free_req: req_id=%d fd=%d", preq->id, req->fd);
  preq->data = NULL;
  preq->in_flight = 0;
  preq->next = processor->reqs_list;
  processor->reqs_list = preq;
  processor->nr_free_reqs++;
}

static inline void processor_print_hash(struct processor *processor,
                                        struct processor_iou_req *wreq,
                                        unsigned char *hash) {
  size_t mlen;
  char debug[OUTPUT_MAX_SIZE];
  mlen = snprintf(
      debug, OUTPUT_MAX_SIZE,
      "{ \"processor\": %d, \"req_id\": %d, \"file\": \"%s\", \"hash\": \"",
      processor->id, wreq->id, (char *)wreq->data);
  static const char hexdigits[] = "0123456789abcdef";
  for (int j = 0; j < HASH_LEN; j++) {
    debug[mlen++] = hexdigits[hash[j] >> 4];
    debug[mlen++] = hexdigits[hash[j] & 0x0f];
  }

  mlen += snprintf(debug + mlen, 5, "\" }\n");
  // TODO: make async
  if (write(1, debug, mlen) != mlen) {
    log_error("error printing result: %s", strerror(errno));
  }
}

static void on_file_read(ev_io_req_t *req) {
  struct processor *processor;
  struct processor_iou_req *wreq;
  unsigned char hash[HASH_LEN];

  log_trace("on_file_read: res=%d", req->result);

  processor = req->data;
  wreq = req->data2;
  if (req->result < 0) {
    log_error("error reading: fd=%d path=%s: %s", req->fd, wreq->data,
              strerror(-req->result));
    goto free;
  }

  hasher_update(&wreq->hasher, req->result, (void *)req->addr);

  if (req->result == 0) {
    hasher_finalize(&wreq->hasher, hash, HASH_LEN);
    processor_print_hash(processor, wreq, hash);
    verifier_verify_hash(&processor->verifier, (size_t)wreq->data2,
                         (char *)wreq->data, HASH_LEN, (char *)&hash);
  free:
    free((char *)wreq->data); // points to pathname string
    wreq->data = NULL;
    wreq->data2 = 0;
    processor_free_req(req);
    close(req->fd); // TODO: make async
    return;
  } else {
    // re-arm
    req->off += req->result;
    ev_iou_req(&processor->loop, req);
  }
}

static void on_file_open(ev_io_req_t *req) {
  struct processor *processor;
  struct processor_iou_req *wreq;

  log_debug("on_file_open: res=%d", req->result);

  processor = req->data;
  wreq = req->data2;
  if (req->result < 0) {
    log_error("error opening %s: %s", wreq->data, strerror(-req->result));
    free((char *)wreq->data); // points to pathname string
    wreq->data = NULL;
    wreq->data2 = 0;
    goto fail;
  }

  // read
  req->opcode = IORING_OP_READ;
  req->fd = req->result;
  req->addr = (uintptr_t)wreq->buffer;
  req->len = READ_BUF_SIZE;
  req->off = 0;
  req->callback = on_file_read;

  hasher_init(&wreq->hasher);
  ev_iou_req(&processor->loop, req);
  return;

fail:
  processor_free_req(req);
}

static void on_queue_event(ev_io_req_t *req) {
  struct processor *processor;
  struct processor_iou_req *preq;
  struct event event;
  ev_io_req_t *open_req;
  uint32_t ret;

  processor = (struct processor *)req->data;
  log_debug("on_queue_event: res=%d", req->result);

  if (req->result < 0) {
    log_error("queue_event error: %s\n", strerror(-req->result));
    if (req->result == -EBADF || req->result == -EINVAL) {
      exit(1);
    }
    processor_submit_poll_reqs(processor);
    return;
  }

  // consume queue
  memset(&event, 0, sizeof(struct event));
  if ((ret = queue_consume_try(&event, req->fd))) {
    if (ret != EAGAIN) {
      log_error("queue_consume returned: %d", ret);
      goto arm;
    }
  }

  // open file
  open_req = processor_get_req(processor);
  preq = open_req->data2;
  open_req->opcode = IORING_OP_OPENAT;
  open_req->fd = AT_FDCWD;
  open_req->addr = (uintptr_t)strdup(event.path);
  open_req->len = 0;
  open_req->flags = O_RDONLY;
  open_req->callback = on_file_open;
  preq->data = (void *)open_req->addr; // pathname
  preq->data2 = (void *)event.path_len;

  if (!open_req->addr) {
    perror("strdup");
    exit(1);
  }

  ev_iou_req(&processor->loop, open_req);
arm:
  if (!(req->result_flags & IORING_CQE_F_MORE)) {
    processor_submit_poll_reqs(processor);
  }
}

static void processor_prepare_poll_reqs(struct processor *processor) {
  // queue events
  ev_io_req_t queue_poll;
  memset(&queue_poll, 0, sizeof(queue_poll));
  queue_poll.opcode = IORING_OP_POLL_ADD;
  queue_poll.fd = processor->queue_efd;
  queue_poll.callback = on_queue_event;
  queue_poll.flags = EPOLLIN | EPOLLEXCLUSIVE;
  queue_poll.len = IORING_POLL_ADD_MULTI;
  queue_poll.data = processor;

  processor->queue_poll_req = queue_poll;
}

// cppcheck-suppress unusedFunction
void *processor_run(void *args) {

  struct processor processor;
  struct processor_args *wargs;

  wargs = (struct processor_args *)args;
  memset(&processor, 0, sizeof(processor));
  processor_setup(&processor, wargs);

  log_error("Processor address is %p", (void *)&processor);
  memset(&processor.verifier, 0, sizeof(struct verifier));
  verifier_init(&processor.verifier, &processor, &processor.loop);

  ev_loop_run(&processor.loop);

  return 0;
}
