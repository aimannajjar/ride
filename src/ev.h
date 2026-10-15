#ifndef EV_H
#define EV_H

#include <stdint.h>

typedef enum { EV_FULL = 1 } ev_err_t;

typedef struct ev_io_req ev_io_req_t;

typedef void (*callback_t)(ev_io_req_t *);

typedef struct ev_io_req {
  uint8_t opcode;
  uint64_t off;
  uint64_t addr;
  int32_t fd;
  uint32_t len;
  uint32_t flags;
  int32_t result;
  uint32_t result_flags;
  void *data;
  void *data2;
  callback_t callback;
} ev_io_req_t;

struct ev__iou_ring {
  uint32_t *head;
  uint32_t *tail;
  uint32_t *mask;
  uint32_t *flags;
};

typedef struct ev_loop {
  struct ev__iou_ring iou_sqe;
  struct ev__iou_ring iou_cqe;
  struct io_uring_sqe *sqes;
  struct io_uring_cqe *cqes;
  int ringfd;

} ev_loop_t;

int ev_iou_req(ev_loop_t *loop, ev_io_req_t *req);
int ev_loop_init(ev_loop_t *loop);
int ev_loop_run(ev_loop_t *loop);

#endif
