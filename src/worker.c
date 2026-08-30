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
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define READ_BUF_SIZE 65536
#define OUTPUT_MAX_SIZE 512
static_assert(!(READ_BUF_SIZE & (4096 - 1)), "BUFFER SIZE must be 4K aligned");

extern atomic_int quit;            // ride.c
extern pthread_mutex_t queue_lock; // queue.c
extern pthread_cond_t queue_cond;  // queue.c

enum task_type {
  HASHING,
  PRINTING,
};

enum task_state {
  AVAILABLE,
  PENDING,
};

struct task_hashing {
  unsigned char hash[HASH_LEN];
  unsigned char path[MAX_FILENAME_LEN];
  struct hasher hasher;
  off_t offset;
  int fd;
};

struct task_printing {
  unsigned char msg[OUTPUT_MAX_SIZE];
  size_t len;
  off_t offset;
};

struct task {
  union {
    struct task_hashing hashing_task;
    struct task_printing printing_task;
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
  fflush(stdout);
  fflush(stderr);
}

// Performs common bookkeeping for new tasks in worker:
//  - updates next availble task id
//  - update available/pending accounts
//
int task_init(struct worker *worker) {
  size_t i, task_slot_idx;
  struct task *task;
  assert(worker->task_slots[worker->next_task_slot].state == AVAILABLE &&
         "Bug: assigned task without available task slot");

  task_slot_idx = worker->next_task_slot;
  task = &worker->task_slots[task_slot_idx];
  task->state = PENDING;

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

void task_hashing_prep_submit(struct worker *worker, size_t tid) {
  struct io_uring_sqe *sqe;
  struct task_hashing *task = &worker->task_slots[tid].as.hashing_task;
  sqe = io_uring_get_sqe(&worker->ring);
  io_uring_prep_read(sqe, task->fd, worker->buffers[tid],
                     sizeof(*worker->buffers), task->offset);
  io_uring_sqe_set_data(sqe, (void *)tid);
  worker->pending_submits++;
}

void task_printing_prep_submit(struct worker *worker, size_t tid) {
  struct io_uring_sqe *sqe;
  struct task_printing *task = &worker->task_slots[tid].as.printing_task;
  sqe = io_uring_get_sqe(&worker->ring);
  io_uring_prep_write(sqe, STDOUT_FILENO, task->msg + task->offset,
                      task->len - task->offset, -1);
  io_uring_sqe_set_data(sqe, (void *)tid);
  worker->pending_submits++;
}

void task_io_submit(struct worker *worker) {
  io_uring_submit(&worker->ring);
  worker->pending_submits = 0;
}

// Performs initialization of new hashing io task
//  - calls base task_init()
//  - initialize buffers
//  - initialize hasher
// struct event must be on the heap
// actual task submission to io_uring is done in task_io_submit
int task_hashing_init(struct worker *worker, struct event *event, int fd) {
  size_t tid;
  struct task *task;

  tid = task_init(worker);
  task = &worker->task_slots[tid];
  task->as.hashing_task.fd = fd;
  task->as.hashing_task.offset = 0;
  task->type = HASHING;
  memset(task->as.hashing_task.hash, 0, sizeof(task->as.hashing_task.hash));
  memcpy(task->as.hashing_task.path, event->path,
         sizeof(task->as.hashing_task.path));
  hasher_init(&task->as.hashing_task.hasher);
  return tid;
}

// Performs initialization of new printing io task
int task_printing_init(struct worker *worker, size_t len, char msg[len]) {
  size_t tid;
  struct task *task;

  tid = task_init(worker);
  task = &worker->task_slots[tid];
  task->type = PRINTING;
  assert(len < sizeof(task->as.printing_task.msg) &&
         "Bug: async output message too large");
  strncpy((char *)task->as.printing_task.msg, msg,
          sizeof(task->as.printing_task.msg));
  task->as.printing_task.msg[sizeof(task->as.printing_task.msg) - 1] = '\0';
  task->as.printing_task.len = len;
  task->as.printing_task.offset = 0;
  return tid;
}

void task_printing_free(struct task *task) {}

void task_hashing_free(struct task *task) { close(task->as.hashing_task.fd); }

void task_free(struct worker *worker, size_t tid) {
  struct task *task = &worker->task_slots[tid];
  switch (task->type) {
  case HASHING:
    task_hashing_free(task);
    break;
  case PRINTING:
    task_printing_free(task);
    break;
  }

  task->state = AVAILABLE;
  worker->next_task_slot = tid;
  worker->pending_tasks--;
  worker->available_slots++;
}

void static inline print_hashing_result(struct worker *worker,
                                        struct task_hashing *task) {
  char debug[OUTPUT_MAX_SIZE];
  size_t tid, mlen;
  mlen = snprintf(debug, OUTPUT_MAX_SIZE,
                  "{ \"worker\": %d, \"file\": \"%s\", \"hash\": \"",
                  worker->id, task->path);
  for (int j = 0; j < HASH_LEN; j++) {
    mlen +=
        snprintf(debug + mlen, OUTPUT_MAX_SIZE - mlen, "%02x", task->hash[j]);
  }
  mlen += snprintf(debug + mlen, 5, "\" }\n");

  // TODO: this should not just assume a task slot is free
  // need enqueuing mechaism
  tid = task_printing_init(worker, mlen, debug);
  task_printing_prep_submit(worker, tid);
}

void *worker_run(void *args) {
  struct worker worker;
  struct worker_args *wargs;

  wargs = (struct worker_args *)args;
  worker_setup(&worker, wargs);
  free(wargs);

  wargs = NULL;
  int new_task_idx;
  while (!atomic_load_explicit(&quit, memory_order_acquire)) {
    int fd;
    struct event event;
    bool new_task = false;

    if (worker.available_slots == worker.nr_tasks) {
      // we have no tasks, block until one is available
      if (queue_consume(&event)) {
        // queue_consume returns 1 if quit was rasied while blocking
        goto done;
      }
      new_task = true;
    } else if (worker.available_slots > 0 &&
               worker.available_slots < worker.nr_tasks) {
      // we have available slots to take more
      new_task = (!queue_consume_try(&event));
    }

    if (new_task) {
      // we've consumed new task, schedule it in async loop
      if ((fd = open(event.path, O_RDONLY)) < 0) {
        // TODO: logging macros
        fprintf(stderr, "Error opening: %s: %s\n", event.path, strerror(errno));
        continue;
      }
      // queue tasks are always hashing kind (so far)
      new_task_idx = task_hashing_init(&worker, &event, fd);
      task_hashing_prep_submit(&worker, new_task_idx);
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
        struct task *task = &worker.task_slots[tid];
        if (cqes[i]->res < 0) {
          // TODO: logging macros
          switch (task->type) {
          case HASHING:
            fprintf(stderr, "(slot %ld) Error async read: %s: %s\n", tid,
                    task->as.hashing_task.path, strerror(-cqes[i]->res));
            break;
          case PRINTING:
            // todo: PRINTING errors
            break;
          }
          task_free(&worker, tid);
          continue;
        } else if (cqes[i]->res > 0) {
          switch (task->type) {
          case HASHING:
            hasher_update(&task->as.hashing_task.hasher, cqes[i]->res,
                          worker.buffers[tid]);

            // read next chunk
            task->as.hashing_task.offset += cqes[i]->res;
            task_hashing_prep_submit(&worker, tid);
            break;
          case PRINTING:
            task->as.printing_task.offset += cqes[i]->res;
            task_printing_prep_submit(&worker, tid);
            break;
          }
        } else {
          // final completion
          switch (task->type) {
          case HASHING:
            hasher_finalize(&task->as.hashing_task.hasher,
                            task->as.hashing_task.hash,
                            sizeof(task->as.hashing_task.hash));

            task_free(&worker, tid);

            // TODO
            // fragile - ref hashing_task after freeing to ensure availble slot
            // restructure after implementing queuing mechanism
            // works now because free doesn't touch hash result (it resets at
            // init)
            print_hashing_result(&worker, &task->as.hashing_task);
            break;
          case PRINTING:
            task_free(&worker, tid);
            break;
          }
        }
      }
      if (worker.pending_submits)
        task_io_submit(&worker);
      io_uring_cq_advance(&worker.ring, n);
    }
  }

done:
  worker_free(&worker);
  return NULL;
}
