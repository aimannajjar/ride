#include "ride.h"
#include "queue.h"
#include <emmintrin.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

struct spinlock {
  atomic_bool flag;
};

static inline void spinlock_lock(struct spinlock *lock) {
  for (;;) {
    while (atomic_load_explicit(&lock->flag, memory_order_relaxed))
      _mm_pause();

    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(&lock->flag, &expected, true,
                                                memory_order_acquire,
                                                memory_order_relaxed))
      return;
  }
}

static inline void spinlock_unlock(struct spinlock *lock) {
  atomic_store_explicit(&lock->flag, false, memory_order_release);
}

struct queue {
  struct event events[QUEUE_SIZE];
  int head;
  int tail;
  alignas(64) struct spinlock lock;
} queue;

void queue_init(void) {
  queue.head = 0;
  queue.tail = 0;
  queue.lock.flag = false;
}

int queue_add(struct event *event) {
  spinlock_lock(&queue.lock);
  if (((queue.head + 1) % QUEUE_SIZE) == queue.tail) {
    spinlock_unlock(&queue.lock);
    fprintf(stderr, "buffer full\n");
    return 1;
  }

  memcpy(&queue.events[queue.head], event, sizeof(struct event));
  queue.head = (queue.head + 1) % QUEUE_SIZE;
  spinlock_unlock(&queue.lock);
  return 0;
}

int queue_consume(struct event *event) {
  spinlock_lock(&queue.lock);

  if (queue.head == queue.tail) {
    spinlock_unlock(&queue.lock);
    return 1;
  }

  memcpy(event, &queue.events[queue.tail], sizeof(struct event));
  queue.tail = (queue.tail + 1) % QUEUE_SIZE;

  spinlock_unlock(&queue.lock);
  return 0;
}
