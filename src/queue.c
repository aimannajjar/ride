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
  pthread_mutex_unlock(&queue_lock);
  log_debug("added to queue: %.*s", event->path_len, event->path);
  return 0;
}

// attempts to consume, if an item is readily available it returns 0
// otherwise 1. Upon consuming, it will read from provided eventfd
int queue_consume_try(struct event *event, int efd) {
  eventfd_t ev = 0;

  log_debug("entering queue_consume_try: efd=%d", efd);
  pthread_mutex_lock(&queue_lock);
  if (eventfd_read(efd, &ev)) {
    if (errno != EAGAIN) {
      perror("eventfd_read queue_efd");
      exit(EXIT_FAILURE);
    } else {
      pthread_mutex_unlock(&queue_lock);
      return EAGAIN;
    }
  }

  if (queue.head == queue.tail) {
    pthread_mutex_unlock(&queue_lock);
    return 1;
  }

  memcpy(event, &queue.events[queue.tail], sizeof(struct event));
  log_debug("queue_consume_try: %s", event->path);
  queue.tail = (queue.tail + 1) % QUEUE_SIZE;
  pthread_mutex_unlock(&queue_lock);
  return 0;
}
