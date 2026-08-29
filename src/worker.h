#ifndef WORKER_H
#define WORKER_H
#include <stdatomic.h>
#include <stddef.h>

#define MAX_CONCURRENT_TASKS 256

extern atomic_int quit;

struct worker_args {
  size_t id;
  size_t io_concurrency;
};

void *worker_run(void *);

#endif // WORKER_H
