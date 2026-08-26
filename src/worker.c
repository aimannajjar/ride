#include "queue.h"
#include "ride.h"
#include "hasher.h"
#include <emmintrin.h>
#include <stdio.h>

#define CONCURRENT_TASKS 2

struct worker {
  int id;
  char tasks[CONCURRENT_TASKS][MAX_FILENAME_LEN];
  struct hasher hashers[CONCURRENT_TASKS];
};

void *worker_run(void *args) {
  struct worker worker;
  worker.id = (long)args;

  struct event event;
  while (1) {
    while (queue_consume(&event)) {
      _mm_pause(); // TODO: portability
    }

    unsigned char out[HASH_LEN];
    hash(&worker.hashers[0], out, event.path);
    printf("WORKER %d PROCESSED: %s = ", worker.id, event.path);
    for (size_t i = 0; i < HASH_LEN; i++)
      printf("%02x", out[i]);
    printf("\n");
  }

  return NULL;
}
