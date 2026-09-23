#ifndef WORKER_H
#define WORKER_H
#include "hasher.h"
#include "ride.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_CONCURRENT_TASKS 256
#define VBG_TIMEOUT 0x1
#define VBG_RECV 0x2

struct worker_args {
  size_t id;             // for logging and debugging
  size_t io_concurrency; // how many concurrent task to execute
  int queue_eventfd;     // eventfd to monitor for queue signals (semaphore)
  bool verify;           // whether verify hashes
};

// Generic jobs can be used by extensions (e.g. verifiers) to have the event
// loop issue callbacks, the callback can specify the next scheduling deadline
// (timeout) and arg to be passed, as well as an fd to add to the event loop
// block/watch poll
typedef int64_t (*job_callback_t)(void *arg, int flags, int *out_sock_fd);
struct job_generic {
  // callback requirements:
  //  1. callback_arg (see below) is passed back as an argument
  //  2. should return 0 if job is completed (e.g. not to be reshceduled)
  //     or non-zero indicating timeout (expiry) for next poll
  //  3. out_sock_fd can be used to register interest in event loop block/wait
  //     operation, can be used for network receives with large timeouts
  job_callback_t callback;

  // most likely you want to pass an instance of the verifier object
  void *callback_arg;
};

// verify jobs
struct job_verify {
  union {
    struct {
      unsigned char hash[HASH_LEN];
      unsigned char path[MAX_FILENAME_LEN];
    } param;
    uint8_t payload[HASH_LEN + MAX_FILENAME_LEN];
  };
  int retries;
};

void *worker_run(void *args);

#endif // WORKER_H
