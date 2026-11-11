#include "queue.h"
#include "log.h"
#include "ride.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/time.h>

extern atomic_int quit; // ride.c
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;

static_assert(QUEUE_SIZE > 0 && !(QUEUE_SIZE & (QUEUE_SIZE - 1)),
              "QUEUE_SIZE must be a power of two");

struct queue {
  struct event events[QUEUE_SIZE];
  alignas(64) atomic_uint head;
  alignas(64) atomic_uint tail;
} queue;

void queue_init(void) {
  queue.head = 0;
  queue.tail = 0;
}

#define IDX(n) (n & (QUEUE_SIZE - 1))

int queue_add(struct event *event) {
  unsigned int head, tail;
  tail = queue.tail;
  head = atomic_load_explicit(&queue.head, memory_order_acquire);
  if (tail - head == QUEUE_SIZE) {
    fprintf(stderr, "buffer full\n");
    return 1;
  }

  memcpy(&queue.events[IDX(tail)], event, sizeof(struct event));
  atomic_store_explicit(&queue.tail, tail + 1, memory_order_release);
  LOG_DEBUG("added to queue: %.*s", event->path_len, event->path, head,
            tail + 1);
  return 0;
}

// attempts to consume, if an item is readily available it returns 0
// otherwise 1. Upon consuming, it will always read from drain eventfd
int queue_consume(struct event *event) {
  unsigned int head, tail;

  LOG_DEBUG("entering queue_consume");

  while (1) {
    tail = atomic_load_explicit(&queue.tail, memory_order_acquire);
    head = atomic_load_explicit(&queue.head, memory_order_relaxed);
    if (IDX(tail) == IDX(head))
      return EAGAIN; // empty

    if (atomic_compare_exchange_strong_explicit(&queue.head, &head, head + 1,
                                                memory_order_release,
                                                memory_order_relaxed)) {

      LOG_DEBUG("consumed event path: %s; head=%d tail=%d", event->path,
                head + 1, tail);
      break;
    }
  }

  memcpy(event, &queue.events[IDX(head)], sizeof(struct event));
  return 0;
}
