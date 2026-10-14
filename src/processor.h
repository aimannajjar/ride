#ifndef WORKER_H
#define WORKER_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_CONCURRENT_TASKS 256
#define VBG_TIMEOUT 0x1
#define VBG_RECV 0x2

struct processor_args {
  size_t id;             // for logging and debugging
  size_t io_concurrency; // how many concurrent task to execute
  int queue_eventfd;     // eventfd to monitor for queue signals (semaphore)
  int quit_eventfd;      // eventfd to monitor for quit signal
  bool verify;
};

void *processor_run(void *args);

#endif // WORKER_H
