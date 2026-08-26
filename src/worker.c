#include "queue.h"
#include "ride.h"
#include <emmintrin.h>
#include <stdio.h>

struct worker {
  int id;
};

void *worker_run(void *args) {

  struct worker worker;
  worker.id = (long)args;

  struct event event;
  while (1) {
    while (queue_consume(&event)) {
      _mm_pause();
    }

    printf("WORKER %d PROCESSED: %s\n", worker.id, event.path);
  }

  return NULL;
}
