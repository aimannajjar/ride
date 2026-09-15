#ifndef WORKER_H
#define WORKER_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_CONCURRENT_TASKS 256

struct worker_args {
  size_t id;
  size_t io_concurrency;
  bool verify;
};

// generic aysnc jobs
// callback: should return 0 if job is completed
//           or non-zero indicating timeout (expiry) for next poll otherwise
typedef uint64_t (*job_callback_t)(void *);

struct job_generic {
  job_callback_t callback;
  void *callback_arg;
};

void *worker_run(void *args);

#endif // WORKER_H
