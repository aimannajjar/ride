#define _GNU_SOURCE

#include "worker.h"
#include "client.h"
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
#define JOB_QUEUE_DEPTH 128 // TODO: make run-time configurable

static_assert(!(READ_BUF_SIZE & (4096 - 1)), "BUFFER SIZE must be 4K aligned");

extern atomic_int quit;            // ride.c
extern pthread_mutex_t queue_lock; // queue.c
extern pthread_cond_t queue_cond;  // queue.c

enum task_type {
  HASHING,
  PRINTING,
};

// encodes parameters specific to hashing jobs
struct job_hashing {
  unsigned char hash[HASH_LEN];
  unsigned char path[MAX_FILENAME_LEN];
  struct hasher hasher;
  off_t offset;
  int fd;
};

// encodes parameters specific to printing jobs
struct job_printing {
  unsigned char msg[OUTPUT_MAX_SIZE];
  size_t len;
  off_t offset;
};

// This struct contain the actual specification
// of job to execute, with union used for type-punning
// into specific specs for various kinds of jobs
struct job {
  union {
    struct job_hashing hashing_job;
    struct job_printing printing_job;
  } as;
  struct job_slot *slot;
  enum task_type type;
  size_t id;
};

// This represents a slot in free/queued jobs list
// each slot points to a job spec, i.e. struct job
struct job_slot {
  struct job job;
  struct job_slot *next;
};

struct task {
  struct job_slot *job_slot;
  size_t id;
};

struct worker {
  // io_uring
  struct io_uring ring;

  // verifier
  struct client verifier;

  // large 4k-aligned buffers indexed by executor (task id)
  unsigned char (*buffers)[READ_BUF_SIZE];

  // jobs
  struct job_slot *free_slots;   // slots are used to schedule jobs
  struct job_slot *queued_slots; // slots that have been scheduled
  struct job_slot *slots;        // unmodified pointer to use for freeing above

  // task (executors)
  struct task *tasks; // represents a task currently in execution
  int *free_tids;     // stack of currently unused executors in tasks list
  int tsp;            // stack pointer for tasks array

  // accounting
  size_t nr_tasks;
  int pending_submits;
  int id;
  bool verify;
};

static void worker_setup(struct worker *worker,
                         const struct worker_args *wargs) {
  ssize_t i;
  worker->id = wargs->id;
  worker->pending_submits = 0;
  worker->nr_tasks = wargs->io_concurrency;
  worker->verify = wargs->verify;
  worker->buffers =
      aligned_alloc(4096, worker->nr_tasks * sizeof(*worker->buffers));

  // initialize verifier
  if (worker->verify)
    client_init(&worker->verifier);

  // initialize tasks satck
  worker->tsp = 0;
  worker->tasks = mallocx(worker->nr_tasks * sizeof(struct job), MALLOCX_ZERO);
  worker->free_tids = malloc(worker->nr_tasks * sizeof(*worker->free_tids));
  for (i = 0; i < worker->nr_tasks; i++) {
    worker->tasks[i].id = i;
    worker->free_tids[i] = i;
  }

  // initilaize jobs lists
  worker->slots = malloc(JOB_QUEUE_DEPTH * sizeof(struct job_slot));
  worker->slots[JOB_QUEUE_DEPTH - 1].next = NULL;
  for (i = JOB_QUEUE_DEPTH - 2; i >= 0; i--) {
    worker->slots[i].next = &worker->slots[i + 1];
  }
  worker->free_slots = worker->slots;
  worker->queued_slots = NULL;

  // setup io_uring
  io_uring_queue_init(worker->nr_tasks, &worker->ring, 0);
}

static void worker_free(struct worker *worker) {
  free(worker->tasks);
  free(worker->buffers);
  free(worker->slots);
  io_uring_queue_exit(&worker->ring);
  fflush(stdout);
  fflush(stderr);
}

// Performs common bookkeeping for new tasks in worker:
//  - updates next availble task id
//  - update available/pending accounts
static struct task *task_take(struct worker *worker) {
  size_t tid;
  struct job_slot *job_slot;
  struct task *task;

  if (worker->tsp == worker->nr_tasks) {
    fprintf(stderr, "not enough executors\n");
    return NULL;
  }

  // take first free scheduled job, and move head to next one
  job_slot = worker->queued_slots;
  if (!job_slot)
    return NULL; // nothing is scheduled

  worker->queued_slots = worker->queued_slots->next;

  tid = worker->free_tids[worker->tsp++];
  task = &worker->tasks[tid];
  task->job_slot = job_slot;
  return task;
}

static void task_hashing_prep_submit(struct worker *worker, struct task *task) {
  struct io_uring_sqe *sqe;
  struct job *job = &task->job_slot->job;
  struct job_hashing *hjob = &job->as.hashing_job;
  sqe = io_uring_get_sqe(&worker->ring);
  io_uring_prep_read(sqe, hjob->fd, worker->buffers[job->id],
                     sizeof(*worker->buffers), hjob->offset);
  io_uring_sqe_set_data(sqe, (void *)task);
  worker->pending_submits++;
}

static void task_printing_prep_submit(struct worker *worker,
                                      struct task *task) {
  struct io_uring_sqe *sqe;
  struct job *job = &task->job_slot->job;
  const struct job_printing *pjob = &job->as.printing_job;
  sqe = io_uring_get_sqe(&worker->ring);
  io_uring_prep_write(sqe, STDOUT_FILENO, pjob->msg + pjob->offset,
                      pjob->len - pjob->offset, -1);
  io_uring_sqe_set_data(sqe, (void *)task);
  worker->pending_submits++;
}

static void task_io_submissions_flush(struct worker *worker) {
  if (worker->pending_submits)
    io_uring_submit(&worker->ring);
  worker->pending_submits = 0;
}

static struct job_slot *enqueue_job(struct worker *worker) {
  // take first available slot and move head
  struct job_slot *job_slot;
  job_slot = worker->free_slots;
  worker->free_slots = worker->free_slots->next;

  // enqueue taken slot in scheduled queue (TODO: consider impact of LIFO)
  job_slot->next = worker->queued_slots;
  worker->queued_slots = job_slot;
  return job_slot;
}

static void enqueue_job_hashing(struct worker *worker,
                                const struct event *event, int fd) {
  struct job_slot *job_slot;
  struct job *job;

  job_slot = enqueue_job(worker);
  job = &job_slot->job;

  // populate job
  job->as.hashing_job.fd = fd;
  job->as.hashing_job.offset = 0;
  job->type = HASHING;
  memset(job->as.hashing_job.hash, 0, sizeof(job->as.hashing_job.hash));
  memcpy(job->as.hashing_job.path, event->path,
         sizeof(job->as.hashing_job.path));
  hasher_init(&job->as.hashing_job.hasher);
}

// Performs initialization of new printing io task
static void enqueue_job_printing(struct worker *worker, size_t len,
                                 const char msg[len]) {
  struct job_slot *job_slot;
  struct job *job;

  job_slot = enqueue_job(worker);
  job = &job_slot->job;

  // populate job
  job->type = PRINTING;
  assert(len < sizeof(job->as.printing_job.msg) &&
         "Bug: async output message too large");
  strncpy((char *)job->as.printing_job.msg, msg,
          sizeof(job->as.printing_job.msg));
  job->as.printing_job.msg[sizeof(job->as.printing_job.msg) - 1] = '\0';
  job->as.printing_job.len = len;
  job->as.printing_job.offset = 0;
}

static void job_printing_free(const struct job *job) {}

static void job_hashing_free(struct job *job) { close(job->as.hashing_job.fd); }

static void task_free(struct worker *worker, struct task *task) {
  struct job_slot *job_slot;
  struct job *job;

  job_slot = task->job_slot;
  job = &task->job_slot->job;
  switch (job->type) {
  case HASHING:
    job_hashing_free(job);
    break;
  case PRINTING:
    job_printing_free(job);
    break;
  }

  // move slot back to free list
  job_slot->next = worker->free_slots;
  worker->free_slots = job_slot;

  // free up task executor and push its id to stack
  task->job_slot = NULL;
  worker->free_tids[--worker->tsp] = task->id;
}

[[maybe_unused]]
void static inline trace_slot_utilization(const struct worker *worker) {
  const struct job_slot *slot;
  size_t free_count, used_count;
  slot = worker->free_slots;
  free_count = 0;
  while (slot) {
    free_count++;
    slot = slot->next;
  }

  used_count = worker->nr_tasks - free_count;
  printf("[SLOT UTILIZATION] Free=%zu; Used=%zu; Ratio=%f\n", free_count,
         used_count, (used_count / (float)worker->nr_tasks) * 100.0);
}

void static inline print_hashing_result(struct worker *worker,
                                        const struct job_hashing *htask) {
  char debug[OUTPUT_MAX_SIZE];
  size_t mlen;

  mlen = snprintf(debug, OUTPUT_MAX_SIZE,
                  "{ \"worker\": %d, \"file\": \"%s\", \"hash\": \"",
                  worker->id, htask->path);
  static const char hexdigits[] = "0123456789abcdef";
  for (int j = 0; j < HASH_LEN; j++) {
    debug[mlen++] = hexdigits[htask->hash[j] >> 4];
    debug[mlen++] = hexdigits[htask->hash[j] & 0x0f];
  }

  mlen += snprintf(debug + mlen, 5, "\" }\n");

  enqueue_job_printing(worker, mlen, debug);
}

static bool have_work(const struct worker *worker) {
  return worker->tsp > 0 || worker->queued_slots;
}

// cppcheck-suppress unusedFunction
void *worker_run(void *args) {
  struct worker worker;
  struct worker_args *wargs;
  [[maybe_unused]] size_t counter = 0;

  wargs = (struct worker_args *)args;
  worker_setup(&worker, wargs);
  free(wargs);
  wargs = NULL;

  counter = 0;
  while (!atomic_load_explicit(&quit, memory_order_acquire)) {
    int fd;
    struct event event;
    bool new_file_event = false;

    if (!have_work(&worker)) {
      // we have no tasks, block until one is available
      if (queue_consume(&event)) {
        // queue_consume returns 1 if quit was rasied while blocking
        goto done;
      }
      new_file_event = true;
    } else if (worker.tsp > 0 && worker.tsp < worker.nr_tasks) {
      new_file_event = (!queue_consume_try(&event));
    }

    if (new_file_event) {
      // we've consumed new task, schedule it in async loop
      if ((fd = open(event.path, O_RDONLY)) < 0) {
        // TODO: logging macros
        fprintf(stderr, "Error opening: %s: %s\n", event.path, strerror(errno));
        continue;
      }

      enqueue_job_hashing(&worker, &event, fd);
    }

    // verifier tasks
    if (worker.verify)
      client_advance(&worker.verifier);

    // Take a task
    struct task *task;
    const struct job *job;
    if ((task = task_take(&worker))) {
      job = &task->job_slot->job;
      switch (job->type) {
      case HASHING:
        task_hashing_prep_submit(&worker, task);
        task_io_submissions_flush(&worker);
        break;
      case PRINTING:
        task_printing_prep_submit(&worker, task);
      }
    }

    // process io_uring completions if any
    struct io_uring_cqe *cqes[worker.nr_tasks];
    size_t i, n;
    n = io_uring_peek_batch_cqe(&worker.ring, cqes, worker.nr_tasks);
    while (!n && worker.tsp == worker.nr_tasks) {
      // when all task executors are waiting on CQEs and none is available
      // block until we are able to make progress
      _mm_pause(); // TODO: portability
      n = io_uring_peek_batch_cqe(&worker.ring, cqes, worker.nr_tasks);
    }

    // process completions
    for (i = 0; i < n; i++) {
      struct task *itask = (struct task *)io_uring_cqe_get_data(cqes[i]);
      struct job *ijob = &itask->job_slot->job;
      if (cqes[i]->res < 0) {
        // TODO: logging macros
        switch (ijob->type) {
        case HASHING:
          fprintf(stderr, "(slot %zu) Error async read: %s: %s\n", itask->id,
                  ijob->as.hashing_job.path, strerror(-cqes[i]->res));
          break;
        case PRINTING:
          // todo: PRINTING errors
          break;
        }
        task_free(&worker, itask);
        continue;
      } else if (cqes[i]->res > 0) {
        switch (ijob->type) {
        case HASHING:
          hasher_update(&ijob->as.hashing_job.hasher, cqes[i]->res,
                        worker.buffers[itask->id]);

          // read next chunk
          ijob->as.hashing_job.offset += cqes[i]->res;
          task_hashing_prep_submit(&worker, itask);
          break;
        case PRINTING:
          ijob->as.printing_job.offset += cqes[i]->res;
          task_printing_prep_submit(&worker, itask);
          break;
        }
      } else {
        // final completion
        switch (ijob->type) {
        case HASHING:
          hasher_finalize(&ijob->as.hashing_job.hasher,
                          ijob->as.hashing_job.hash,
                          sizeof(ijob->as.hashing_job.hash));

          task_free(&worker, itask);

          print_hashing_result(&worker, &ijob->as.hashing_job);
          break;
        case PRINTING:
          task_free(&worker, itask);
          break;
        }
      }
    }

    task_io_submissions_flush(&worker);
    io_uring_cq_advance(&worker.ring, n);

#ifdef USERSPACE_TRACE
    if ((counter % 10000) == 0)
      trace_slot_utilization(&worker);
#endif
    counter++;
  }

done:
  worker_free(&worker);
  return NULL;
}
