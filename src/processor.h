#ifndef WORKER_H
#define WORKER_H
#include "ev.h"
#include "verifier.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#define READ_BUF_SIZE 65536
#define OUTPUT_MAX_SIZE 512
#define MAX_CONCURRENT_TASKS 256
#define VBG_TIMEOUT 0x1
#define VBG_RECV 0x2

struct processor {
  int queue_efd; // eventfd for main thread producer notifies (passed as warg)

  // verifier
  struct verifier verifier;

  // large 4k-aligned buffers indexed by executor (task id)
  unsigned char (*buffers)[READ_BUF_SIZE];
  struct processor_iou_req *reqs;      // pre-allocated slab for requests
  struct processor_iou_req *reqs_list; // linked list to access the slab
  ssize_t nr_free_reqs;

  // event loop
  ev_io_req_t queue_poll_req;
  ev_loop_t loop;

  // accounting
  size_t nr_tasks;
  int id;
};

struct processor_args {
  size_t id;             // for logging and debugging
  size_t io_concurrency; // how many concurrent task to execute
  int queue_eventfd;     // eventfd to monitor for queue signals (semaphore)
  int quit_eventfd;      // eventfd to monitor for quit signal
  bool verify;
};

void *processor_run(void *args);
void processor_free_req(ev_io_req_t *req);
ev_io_req_t *processor_get_req(struct processor *processor);

#endif // WORKER_H
