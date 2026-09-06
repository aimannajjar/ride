#ifndef WORKER_H
#define WORKER_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_CONCURRENT_TASKS 256

struct worker_args {
  size_t id;
  size_t io_concurrency;
  bool verify;
};

void *worker_run(void *args);

#endif // WORKER_H
