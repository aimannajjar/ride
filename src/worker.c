#define _GNU_SOURCE

#include "worker.h"
#include "hasher.h"
#include "queue.h"
#include "ride.h"
#include <assert.h>
#include <emmintrin.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_CONCURRENT_TASKS 32
#define READ_BUF_SIZE 4096

enum task_state {
  AVAILABLE,
  READING,
};

struct worker {
  // io_uring
  struct io_uring ring;
  size_t io_tasks;

  // per-task SoAs to keep elements nearby in cache line during async event loop
  unsigned char read_buffers[MAX_CONCURRENT_TASKS][READ_BUF_SIZE];
  unsigned char hashes[MAX_CONCURRENT_TASKS][HASH_LEN];
  char *task_paths[MAX_CONCURRENT_TASKS];
  int fds[MAX_CONCURRENT_TASKS];
  off_t offsets[MAX_CONCURRENT_TASKS];
  enum task_state states[MAX_CONCURRENT_TASKS];
  struct hasher hashers[MAX_CONCURRENT_TASKS];

  int pending_tasks;
  int next_task_idx;
  int id;
};

void worker_setup(struct worker *worker, struct worker_args *wargs) {
  worker->pending_tasks = 0;
  worker->next_task_idx = 0;
  worker->io_tasks = wargs->io_concurrency;

  size_t i;
  for (i = 0; i < worker->io_tasks; i++) {
    worker->states[i] = AVAILABLE;
  }

  // setup io_uring
  io_uring_queue_init(worker->io_tasks, &worker->ring, 0);
}

// Performs bookkeeping for tasks in worker:
//  - updates next availble task id
//  - initialize buffers
//  - initialize hasher
// struct event must be on the heap
// actual task submission to io_uring is done in task_io_submit
int task_init(struct worker *worker, struct event *event, int fd) {
  size_t i, my_index;
  assert(worker->states[worker->next_task_idx] == AVAILABLE);
  my_index = worker->next_task_idx;
  worker->task_paths[my_index] = event->path;
  worker->fds[my_index] = fd;
  worker->offsets[my_index] = 0;
  worker->states[my_index] = READING;
  memset(worker->read_buffers[my_index], 0, sizeof(worker->read_buffers[0]));
  memset(worker->hashes[my_index], 0, sizeof(worker->hashes[0]));
  hasher_init(&worker->hashers[my_index]);
  worker->pending_tasks++;

  // find next available task
  if (worker->pending_tasks < worker->io_tasks) {
    for (i = 0; i < worker->io_tasks; i++) {
      if (worker->states[i] == AVAILABLE) {
        worker->next_task_idx = i;
        return my_index;
      }
    }
    assert(false && "Bug: pending_tasks / states discrepancy");
  }

  return my_index;
}

// submits a given task to io_uring
// can be reused to read chunks
void task_io_submit(struct worker *worker, size_t index) {
  struct io_uring_sqe *sqe;
  sqe = io_uring_get_sqe(&worker->ring);
  io_uring_prep_read(sqe, worker->fds[index], worker->read_buffers[index],
                     sizeof(worker->read_buffers[0]), worker->offsets[index]);
  io_uring_sqe_set_data(sqe, (void *)index);
  io_uring_submit(&worker->ring);
}

void task_free(struct worker *worker, size_t index) {
  if (worker->task_paths[index])
    free(worker->task_paths[index]);
  worker->task_paths[index] = NULL;
  worker->states[index] = AVAILABLE;
  worker->next_task_idx = index;
  worker->pending_tasks--;
}

void *worker_run(void *args) {
  struct worker worker;
  struct worker_args *wargs;

  wargs = (struct worker_args *)args;
  worker_setup(&worker, wargs);
  free(wargs);

  wargs = NULL;
  bool new_task;
  int new_task_idx;
  while (1) {
    int fd;
    struct event *event = malloc(sizeof(struct event));
    bool new_task = false;

    if (worker.pending_tasks == 0) {
      // we have no tasks, block until one is available
      while (queue_consume(event)) {
        _mm_pause(); // TODO: portability
      }
      new_task = true;
    } else if (worker.pending_tasks < worker.io_tasks) {
      // we have pending tasks, but we can take more, take one
      queue_consume(event);
      new_task = true;
    }

    if (new_task) {
      // we've consumed new task, schedule it in async loop
      if ((fd = open(event->path, O_RDONLY | O_DIRECT)) < 0) {
        // TODO: logging macros
        fprintf(stderr, "Error opening: %s: %s\n", event->path,
                strerror(errno));
        free(event);
        event = NULL;
        continue;
      }
      new_task_idx = task_init(&worker, event, fd);
      task_io_submit(&worker, new_task_idx);
    }

    while (worker.pending_tasks) {
      struct io_uring_cqe *cqes[worker.io_tasks];
      size_t i, n;
      n = io_uring_peek_batch_cqe(&worker.ring, cqes, worker.io_tasks);

      // process completions
      for (i = 0; i < n; i++) {
        size_t task_idx = (size_t)io_uring_cqe_get_data(cqes[i]);
        if (cqes[i]->res < 0) {
          // TODO: logging macros
          fprintf(stderr, "Error async read: %s: %s\n",
                  worker.task_paths[task_idx], strerror(errno));
          task_free(&worker, task_idx);
          continue;
        } else if (cqes[i]->res > 0) {
          hasher_update(&worker.hashers[task_idx], cqes[i]->res,
                        worker.read_buffers[task_idx]);

          // read next chunk
          worker.offsets[task_idx] += cqes[i]->res;
          task_io_submit(&worker, task_idx);
        } else {
          hasher_finalize(&worker.hashers[task_idx], worker.hashes[task_idx],
                          HASH_LEN);

#ifdef USERSPACE_DEBUG
          char debug[1024];
          int mlen;
          mlen = snprintf(debug, 1024, "WORKER %d PROCESSED: %s = ", worker.id,
                          worker.task_paths[task_idx]);
          for (i = 0; i < HASH_LEN; i++) {
            mlen += snprintf(debug + mlen, 512 - mlen, "%02x",
                             worker.hashes[task_idx][i]);
          }
          printf("%s\n", debug);
#endif
          task_free(&worker, task_idx);
        }
      }

      io_uring_cq_advance(&worker.ring, n);
    }
  }

  return NULL;
}

