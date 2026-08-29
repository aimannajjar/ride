#define _GNU_SOURCE

#include "worker.h"
#include "hasher.h"
#include "queue.h"
#include "ride.h"
#include <assert.h>
#include <emmintrin.h>
#include <fcntl.h>
#include <jemalloc/jemalloc.h>
#include <liburing.h>
#include <stdalign.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define READ_BUF_SIZE 65536
static_assert(!(READ_BUF_SIZE & (4096 - 1)), "BUFFER SIZE must be 4K aligned");

enum task_type {
  HASHING,
};

enum task_state {
  AVAILABLE,
  READING,
};

struct task_hashing {
  unsigned char hash[HASH_LEN];
  unsigned char path[MAX_FILENAME_LEN];
  struct hasher hasher;
  off_t offset;
  int fd;
};

struct task {
  union {
    struct task_hashing hashing_task;
  } as;
  enum task_type type;
  enum task_state state;
};

struct worker {
  // io_uring
  struct io_uring ring;
  size_t nr_tasks;

  unsigned char (*buffers)[READ_BUF_SIZE];

  struct task *task_slots;
  int available_slots;
  int pending_submits;
  int pending_tasks;
  int next_task_slot;
  int id;
};

void worker_setup(struct worker *worker, struct worker_args *wargs) {
  worker->id = wargs->id;
  worker->pending_tasks = 0;
  worker->pending_submits = 0;
  worker->next_task_slot = 0;
  worker->nr_tasks = wargs->io_concurrency;
  worker->available_slots = worker->nr_tasks;
  worker->task_slots =
      mallocx(worker->nr_tasks * sizeof(struct task), MALLOCX_ZERO);
  worker->buffers =
      aligned_alloc(4096, worker->nr_tasks * sizeof(*worker->buffers));
  size_t i;
  for (i = 0; i < worker->nr_tasks; i++) {
    worker->task_slots[i].state = AVAILABLE;
  }

  // setup io_uring
  io_uring_queue_init(worker->nr_tasks, &worker->ring, 0);
}

void worker_free(struct worker *worker) {
  free(worker->task_slots);
  free(worker->buffers);
  io_uring_queue_exit(&worker->ring);
}

// Performs bookkeeping for tasks in worker:
//  - updates next availble task id
//  - initialize buffers
//  - initialize hasher
// struct event must be on the heap
// actual task submission to io_uring is done in task_io_submit
int task_init(struct worker *worker, struct event *event, int fd) {
  size_t i, task_slot_idx;
  struct task *task;
  assert(worker->task_slots[worker->next_task_slot].state == AVAILABLE &&
         "Bug: assigned task without available task slot");

  task_slot_idx = worker->next_task_slot;
  task = &worker->task_slots[task_slot_idx];
  task->as.hashing_task.fd = fd;
  task->as.hashing_task.offset = 0;
  task->state = READING;
  task->type = HASHING;

  memset(task->as.hashing_task.hash, 0, sizeof(task->as.hashing_task.hash));
  memcpy(task->as.hashing_task.path, event->path,
         sizeof(task->as.hashing_task.path));
  hasher_init(&task->as.hashing_task.hasher);
  worker->available_slots--;
  worker->pending_tasks++;

  // find next available task
  if (worker->available_slots > 0) {
    for (i = 0; i < worker->nr_tasks; i++) {
      if (worker->task_slots[i].state == AVAILABLE) {
        worker->next_task_slot = i;
        return task_slot_idx;
      }
    }
    assert(false && "Bug: available_slots / states discrepancy");
  }

  return task_slot_idx;
}

// submits a given task to io_uring
// can be reused to read chunks
void task_io_prep_submit(struct worker *worker, size_t tid) {
  struct io_uring_sqe *sqe;
  struct task_hashing *task = &worker->task_slots[tid].as.hashing_task;
  sqe = io_uring_get_sqe(&worker->ring);
  io_uring_prep_read(sqe, task->fd, worker->buffers[tid],
                     sizeof(*worker->buffers), task->offset);
  io_uring_sqe_set_data(sqe, (void *)tid);
  worker->pending_submits++;
}

void task_io_submit(struct worker *worker) {
  io_uring_submit(&worker->ring);
  worker->pending_submits = 0;
}

void task_free(struct worker *worker, size_t index) {
  struct task *task = &worker->task_slots[index];
  task->as.hashing_task.path[0] = '\0';
  task->state = AVAILABLE;
  close(task->as.hashing_task.fd);
  worker->next_task_slot = index;
  worker->pending_tasks--;
  worker->available_slots++;
}

void *worker_run(void *args) {
  struct worker worker;
  struct worker_args *wargs;

  wargs = (struct worker_args *)args;
  worker_setup(&worker, wargs);
  free(wargs);

  wargs = NULL;
  int new_task_idx;
  while (1) {
    int fd;
    struct event event;
    bool new_task = false;

    if (worker.available_slots == worker.nr_tasks) {
      // we have no tasks, block until one is available
      while (queue_consume(&event)) {
        _mm_pause(); // TODO: portability
      }
      new_task = true;
    } else if (worker.available_slots > 0 &&
               worker.available_slots < worker.nr_tasks) {
      // we have available slots to take more
      new_task = (!queue_consume(&event));
    }

    if (new_task) {
      // we've consumed new task, schedule it in async loop
      if ((fd = open(event.path, O_RDONLY)) < 0) {
        // TODO: logging macros
        fprintf(stderr, "Error opening: %s: %s\n", event.path, strerror(errno));
        continue;
      }
      new_task_idx = task_init(&worker, &event, fd);
      task_io_prep_submit(&worker, new_task_idx);
      task_io_submit(&worker);
    }

    if (worker.pending_tasks) {
      struct io_uring_cqe *cqes[worker.nr_tasks];
      size_t i, n;
      n = io_uring_peek_batch_cqe(&worker.ring, cqes, worker.nr_tasks);
      while (!n && !worker.available_slots) {
        // when all slots are waiting on CQEs and none is available
        // pause and retry
        _mm_pause();
        n = io_uring_peek_batch_cqe(&worker.ring, cqes, worker.nr_tasks);
      }

      // process completions
      for (i = 0; i < n; i++) {
        size_t tid = (size_t)io_uring_cqe_get_data(cqes[i]);
        struct task_hashing *task = &worker.task_slots[tid].as.hashing_task;
        if (cqes[i]->res < 0) {
          // TODO: logging macros
          fprintf(stderr, "Error async read: %s: %s\n", task->path,
                  strerror(-cqes[i]->res));
          task_free(&worker, tid);
          continue;
        } else if (cqes[i]->res > 0) {
          hasher_update(&task->hasher, cqes[i]->res, worker.buffers[tid]);

          // read next chunk
          task->offset += cqes[i]->res;
          task_io_prep_submit(&worker, tid);
        } else {
          hasher_finalize(&task->hasher, task->hash, sizeof(task->hash));

#ifdef USERSPACE_DEBUG
          char debug[1024];
          int mlen;
          mlen = snprintf(debug, 1024,
                          "{ \"worker\": %d, \"file\": \"%s\", \"hash\": \"",
                          worker.id, task->path);
          for (int j = 0; j < HASH_LEN; j++) {
            mlen += snprintf(debug + mlen, 512 - mlen, "%02x", task->hash[j]);
          }
          printf("%s\" }\n", debug);
#endif
          task_free(&worker, tid);
        }
      }
      if (worker.pending_submits)
        task_io_submit(&worker);
      io_uring_cq_advance(&worker.ring, n);
    }
  }

  worker_free(&worker);
  return NULL;
}
