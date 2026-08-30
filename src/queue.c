#include "queue.h"
#include "ride.h"
#include <emmintrin.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

struct spinlock {
  atomic_bool flag;
};

extern atomic_int quit; // ride.c
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

struct queue {
  struct event events[QUEUE_SIZE];
  int head;
  int tail;
} queue;

void queue_init(void) {
  queue.head = 0;
  queue.tail = 0;
}

int queue_add(struct event *event) {
  pthread_mutex_lock(&queue_lock);
  if (((queue.head + 1) % QUEUE_SIZE) == queue.tail) {
    pthread_mutex_unlock(&queue_lock);
    fprintf(stderr, "buffer full\n");
    return 1;
  }

  memcpy(&queue.events[queue.head], event, sizeof(struct event));
  queue.head = (queue.head + 1) % QUEUE_SIZE;
  pthread_cond_signal(&queue_cond);
  pthread_mutex_unlock(&queue_lock);
  return 0;
}

// attempts to consume, if an item is readily available it returns 0
// otherwise 1
int queue_consume_try(struct event *event) {
  pthread_mutex_lock(&queue_lock);

  if (queue.head == queue.tail) {
    pthread_mutex_unlock(&queue_lock);
    return 1;
  }

  memcpy(event, &queue.events[queue.tail], sizeof(struct event));
  queue.tail = (queue.tail + 1) % QUEUE_SIZE;

  pthread_mutex_unlock(&queue_lock);
  return 0;
}

// blocks when queue is empty
// unless global flag `quit` is raised, at which point it returns 1
int queue_consume(struct event *event) {
  pthread_mutex_lock(&queue_lock);

  while (queue.head == queue.tail && !quit) {
    pthread_cond_wait(&queue_cond, &queue_lock);
  }

  if (quit) {
    pthread_mutex_unlock(&queue_lock);
    return 1;
  }

  memcpy(event, &queue.events[queue.tail], sizeof(struct event));
  queue.tail = (queue.tail + 1) % QUEUE_SIZE;

  pthread_mutex_unlock(&queue_lock);
  return 0;
}
