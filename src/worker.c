#include <asm-generic/errno.h>
#include <stdint.h>
#define _GNU_SOURCE

#include "client.h"
#include "hasher.h"
#include "queue.h"
#include "ride.h"
#include "worker.h"
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
#define DEFAULT_TIMEOUT 300

static_assert(!(READ_BUF_SIZE & (4096 - 1)), "BUFFER SIZE must be 4K aligned");

extern atomic_int quit;            // ride.c
extern pthread_mutex_t queue_lock; // queue.c
extern pthread_cond_t queue_cond;  // queue.c

enum job_type {
  HASH,
  PRINT,
  VERIFY,
  GENERIC,
};

enum task_state {
  IDLE,
  RUNNING,
};

// encodes parameters specific to hash jobs
struct job_hash {
  unsigned char hash[HASH_LEN];
  unsigned char path[MAX_FILENAME_LEN];
  struct hasher hasher;
  off_t offset;
  int fd;
};

// encodes parameters specific to print jobs
struct job_print {
  unsigned char msg[OUTPUT_MAX_SIZE];
  size_t len;
  off_t offset;
};

// verify jobs
struct job_verify {
  unsigned char hash[HASH_LEN];
  unsigned char path[MAX_FILENAME_LEN];
};

// This struct contain the actual specification
// of job to execute, with union used for type-punning
// into specific specs for various kinds of jobs
struct job {
  union {
    struct job_hash hash_job;
    struct job_print print_job;
    struct job_verify verify_job;
    struct job_generic generic_job;
  } as;
  struct job_slot *slot;
  enum job_type type;
  size_t id;
};

// This represents a slot in free/queued jobs list
// each slot points to a job spec, i.e. struct job
struct job_slot {
  size_t id;
  struct job job;
  struct job_slot *next;
  struct job_slot *prev; // only used when in queued list
};

// Task representts a job that's currently being executed
// - this is what counts toward concurrency depth
// - tasks are preallocated (see struct worker), but they point to different
//   job_slots when new jobs are assigned and completed
struct task {
  struct job_slot *job_slot;
  enum task_state state;
  size_t id;
};

struct worker {
  // io_uring
  struct io_uring ring;

  // verifier
  struct client verifier;
  struct verifier_config verifier_config;

  // large 4k-aligned buffers indexed by executor (task id)
  unsigned char (*buffers)[READ_BUF_SIZE];

  // jobs
  struct job_slot *free_slots;  // slots are used to schedule jobs
  struct job_slot *queued_head; // slots that have been scheduled
  struct job_slot *queued_tail;
  struct job_slot *slots; // unmodified pointer to use for freeing above

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

[[maybe_unused]]
void static inline trace_print_tasks(const struct worker *worker) {
  struct task *task;
  ssize_t i;

  for (i = 0; i < worker->nr_tasks; i++) {
    task = &worker->tasks[i];
    if (task->state == RUNNING)
      printf("task %ld, slot id: %zd, type: %d\n", task->id, task->job_slot->id,
             task->job_slot->job.type);
  }
}

[[maybe_unused]]
void static inline trace_print_queued(const struct worker *worker) {
  struct job_slot *slot;

  slot = worker->queued_head;
  while (slot) {
    slot = slot->next;
  }
}

// Adds a job to queue list
// if job_slot is NULL, it will pick a job slot from free list
// if job_slot is specified, it should have been acquired previously
// from enqueue_job
static struct job_slot *enqueue_job(struct worker *worker,
                                    struct job_slot *job_slot) {
  // take first available slot and move head
  if (!job_slot) {
    job_slot = worker->free_slots;
    worker->free_slots = worker->free_slots->next;
  }

  if (!worker->queued_head) {
    worker->queued_head = job_slot;
    worker->queued_tail = job_slot;
    job_slot->next = NULL;
    job_slot->prev = NULL;
  } else {
    job_slot->prev = worker->queued_tail;
    job_slot->next = NULL;
    worker->queued_tail->next = job_slot;
    worker->queued_tail = job_slot;
  }

  return job_slot;
}

// Pops first element from queue list
// Note, the caller must return it to free list by calling return job
// or alternatively call requeue to add it back to queue list
static struct job_slot *dequeue_job(struct worker *worker) {
  struct job_slot *job_slot;

  // take first free queued job, and move head to next one
  if (!worker->queued_head)
    return NULL; // nothing is queued

  // move head
  job_slot = worker->queued_head;
  worker->queued_head = worker->queued_head->next;
  if (worker->queued_head)
    worker->queued_head->prev = NULL;

  return job_slot;
}

// returns a job slot to free list
static void return_job(struct worker *worker, struct job_slot *job_slot) {
  job_slot->next = worker->free_slots;
  job_slot->prev = NULL; // free list is singly linked list
  worker->free_slots = job_slot;
}

// This has the same effect as calling enqueu_job and passing job_slot
static void requeue_job(struct worker *worker, struct job_slot *job_slot) {
  enqueue_job(worker, job_slot);
}

static void worker_setup(struct worker *worker,
                         const struct worker_args *wargs) {
  ssize_t i;
  worker->id = wargs->id;
  worker->pending_submits = 0;
  worker->nr_tasks = wargs->io_concurrency;
  worker->verify = wargs->verify;
  worker->buffers =
      aligned_alloc(4096, worker->nr_tasks * sizeof(*worker->buffers));

  // initialize tasks satck
  worker->tsp = 0;
  worker->tasks = mallocx(worker->nr_tasks * sizeof(struct job), MALLOCX_ZERO);
  worker->free_tids = malloc(worker->nr_tasks * sizeof(*worker->free_tids));
  for (i = 0; i < worker->nr_tasks; i++) {
    worker->tasks[i].id = i;
    worker->tasks[i].state = IDLE;
    worker->free_tids[i] = i;
  }

  // initilaize jobs lists
  worker->slots = malloc(JOB_QUEUE_DEPTH * sizeof(struct job_slot));
  worker->slots[JOB_QUEUE_DEPTH - 1].next = NULL;
  worker->slots[JOB_QUEUE_DEPTH - 1].id = JOB_QUEUE_DEPTH - 1;
  for (i = JOB_QUEUE_DEPTH - 2; i >= 0; i--) {
    worker->slots[i].next = &worker->slots[i + 1];
    worker->slots[i].id = i;
  }
  worker->free_slots = worker->slots;
  worker->queued_head = NULL;
  worker->queued_tail = NULL;

  // setup io_uring
  io_uring_queue_init(worker->nr_tasks, &worker->ring, 0);

  // initialize verifier
  struct job_slot *verifier_bg_job_slot;
  struct job_generic *verifier_bg_job;
  if (worker->verify) {
    worker->verifier_config = client_verifier_config();
    verifier_bg_job_slot = NULL;
    verifier_bg_job = NULL;
    if (worker->verifier_config.requires_background_job) {
      verifier_bg_job_slot = enqueue_job(worker, NULL);
      verifier_bg_job_slot->job.type = GENERIC;
      verifier_bg_job = &verifier_bg_job_slot->job.as.generic_job;
      verifier_bg_job->callback = NULL;
      verifier_bg_job->callback_arg = NULL;
    }
    client_init(&worker->verifier, verifier_bg_job);
    assert(
        !worker->verifier_config.requires_background_job ||
        (worker->verifier_config.requires_background_job &&
         verifier_bg_job->callback != NULL) &&
            "verifier requires background job but does not specify it in init");
  }
}

static void worker_free(struct worker *worker) {
  free(worker->tasks);
  free(worker->buffers);
  free(worker->slots);
  io_uring_queue_exit(&worker->ring);
  fflush(stdout);
  fflush(stderr);
}

static struct task *task_take(struct worker *worker) {
  size_t tid;
  struct job_slot *job_slot;
  struct task *task;
  if (worker->tsp == worker->nr_tasks) {
    fprintf(stderr, "warning: not enough executors\n");
    return NULL;
  }

  if (!(job_slot = dequeue_job(worker)))
    return NULL;

  tid = worker->free_tids[worker->tsp++];
  task = &worker->tasks[tid];
  task->job_slot = job_slot;
  task->state = RUNNING;
  return task;
}

static void task_hash_prep_submit(struct worker *worker, struct task *task) {
  struct io_uring_sqe *sqe;
  struct job *job = &task->job_slot->job;
  struct job_hash *hjob = &job->as.hash_job;
  sqe = io_uring_get_sqe(&worker->ring);
  io_uring_prep_read(sqe, hjob->fd, worker->buffers[job->id],
                     sizeof(*worker->buffers), hjob->offset);
  io_uring_sqe_set_data(sqe, (void *)task);
  worker->pending_submits++;
}

static void task_print_prep_submit(struct worker *worker, struct task *task) {
  struct io_uring_sqe *sqe;
  struct job *job = &task->job_slot->job;
  const struct job_print *pjob = &job->as.print_job;
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

void enqueue_job_verify(struct worker *worker, const struct event *event,
                        int fd) {
  struct job_slot *job_slot;
  struct job *job;

  job_slot = enqueue_job(worker, NULL);
  job = &job_slot->job;

  // populate job
  job->as.hash_job.fd = fd;
  job->as.hash_job.offset = 0;
  job->type = HASH;
  memset(job->as.hash_job.hash, 0, sizeof(job->as.hash_job.hash));
  memcpy(job->as.hash_job.path, event->path, sizeof(job->as.hash_job.path));
  hasher_init(&job->as.hash_job.hasher);
}

static void enqueue_job_hash(struct worker *worker, const struct event *event,
                             int fd) {
  struct job_slot *job_slot;
  struct job *job;

  job_slot = enqueue_job(worker, NULL);
  job = &job_slot->job;

  // populate job
  job->as.hash_job.fd = fd;
  job->as.hash_job.offset = 0;
  job->type = HASH;
  memset(job->as.hash_job.hash, 0, sizeof(job->as.hash_job.hash));
  memcpy(job->as.hash_job.path, event->path, sizeof(job->as.hash_job.path));
  hasher_init(&job->as.hash_job.hasher);
}

// Performs initialization of new printing io task
static void enqueue_job_print(struct worker *worker, size_t len,
                              const char msg[len]) {
  struct job_slot *job_slot;
  struct job *job;

  job_slot = enqueue_job(worker, NULL);
  job = &job_slot->job;

  // populate job
  job->type = PRINT;
  assert(len < sizeof(job->as.print_job.msg) &&
         "Bug: async output message too large");
  strncpy((char *)job->as.print_job.msg, msg, sizeof(job->as.print_job.msg));
  job->as.print_job.msg[sizeof(job->as.print_job.msg) - 1] = '\0';
  job->as.print_job.len = len;
  job->as.print_job.offset = 0;
}

static void job_print_free(struct job *job) {}

static void job_generic_free(struct job *job) {
  job->as.generic_job.callback_arg = NULL;
  job->as.generic_job.callback = NULL;
};

static void job_hash_free(struct job *job) { close(job->as.hash_job.fd); }

static void task_reschedule(struct worker *worker, struct task *task) {
  struct job_slot *job_slot;
  job_slot = task->job_slot;

  // free up task executor and push its id to stack
  task->job_slot = NULL;
  task->state = IDLE;
  worker->free_tids[--worker->tsp] = task->id;

  // put the job back on queue list to reschedule it
  requeue_job(worker, job_slot);
}

static void task_free(struct worker *worker, struct task *task) {
  struct job_slot *job_slot;
  struct job *job;

  job_slot = task->job_slot;
  job = &task->job_slot->job;
  switch (job->type) {
  case HASH:
    job_hash_free(job);
    break;
  case PRINT:
    job_print_free(job);
    break;
  case GENERIC:
    job_generic_free(job);
  case VERIFY:
    assert(false && "not implemented");
    break;
  }

  // move slot back from queued to free list
  return_job(worker, job_slot);

  // free up task executor and push its id to stack
  task->job_slot = NULL;
  task->state = IDLE;
  worker->free_tids[--worker->tsp] = task->id;
}

void static inline print_hash_result(struct worker *worker,
                                     const struct job_hash *htask) {
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

  enqueue_job_print(worker, mlen, debug);
}

// Returns true if there are not tasks in execution and also no jobs in our
// job queue. This excludes verifier background job which is always running
// This can be used by callers to block until jobs are available to enqueue
static bool need_work(const struct worker *worker) {
  // because it always runs, we are interested in learning whether there is
  // new work that is "completable"
  bool bg = (worker->verify && worker->verifier_config.requires_background_job);

  bool jobs_queued = worker->queued_head &&
                     (worker->queued_head != worker->queued_tail || !bg);

  // tsp > 0 means we have tasks still executing
  return worker->tsp == 0 && !jobs_queued;
}

// useful when there is no pointing iterating because we're entirely blocked
// by io_uring
static bool all_tasks_in_io_uring(const struct worker *worker) {
  size_t capacity =
      (worker->verify && worker->verifier_config.requires_background_job)
          ? worker->nr_tasks - 1
          : worker->nr_tasks;

  return worker->tsp == capacity;
}

// cppcheck-suppress unusedFunction
void *worker_run(void *args) {
  struct worker worker;
  struct worker_args *wargs;
  uint64_t timeout;

  wargs = (struct worker_args *)args;
  worker_setup(&worker, wargs);
  free(wargs);
  wargs = NULL;

  timeout = DEFAULT_TIMEOUT;
  while (!atomic_load_explicit(&quit, memory_order_acquire)) {
    int fd;
    int ret;
    struct event event;
    bool new_file_event = false;

    if (need_work(&worker)) {
      // we have no tasks, poll
      if ((ret = queue_consume(&event, 1000)) == ESHUTDOWN) {
        goto done;
      } else if (ret != ETIMEDOUT) {
        new_file_event = true;
      }
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

      enqueue_job_hash(&worker, &event, fd);
    }

    // Take a task
    struct task *task;
    const struct job *job;
    if ((task = task_take(&worker))) {
      job = &task->job_slot->job;
      switch (job->type) {
      case HASH:
        task_hash_prep_submit(&worker, task);
        task_io_submissions_flush(&worker);
        break;
      case PRINT:
        task_print_prep_submit(&worker, task);
        break;
      case GENERIC:
        assert(job->as.generic_job.callback &&
               "GENERIC job callback undefined");
        // ideally we want minimum timeout of all jobs but currently we only
        // have one job that publishes a timeout (verifier bg job)
        if (!(timeout = job->as.generic_job.callback(
                  job->as.generic_job.callback_arg))) {
          // when generic job returns zero, it means it completed
          timeout = DEFAULT_TIMEOUT;
          task_free(&worker, task);
          printf("freed generic job\n");
        } else {
          // non zero return, means task want to be rescheduled with returned
          // timeout
          task_reschedule(&worker, task);
        }
        break;
      default:
        assert(false && "not implemented");
        break;
      }
    }

    // process io_uring completions if any
    struct io_uring_cqe *cqes[worker.nr_tasks];
    size_t i, n;
    n = io_uring_peek_batch_cqe(&worker.ring, cqes, worker.nr_tasks);
    while (!n && all_tasks_in_io_uring(&worker)) {
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
        case HASH:
          fprintf(stderr, "(slot %zu) Error async read: %s: %s\n", itask->id,
                  ijob->as.hash_job.path, strerror(-cqes[i]->res));
          break;
        case PRINT:
          // todo: PRINTING errors
          break;
        default:
          // handle any future tasks that are submitted through io uring here
          assert(false && "unreachable");
          break;
        }
        task_free(&worker, itask);
        continue;
      } else if (cqes[i]->res > 0) {
        switch (ijob->type) {
        case HASH:
          hasher_update(&ijob->as.hash_job.hasher, cqes[i]->res,
                        worker.buffers[itask->id]);

          // read next chunk
          ijob->as.hash_job.offset += cqes[i]->res;
          task_hash_prep_submit(&worker, itask);
          break;
        case PRINT:
          ijob->as.print_job.offset += cqes[i]->res;
          task_print_prep_submit(&worker, itask);
          break;
        default:
          assert(false && "unreachable");
          break;
        }
      } else {
        // final completion
        switch (ijob->type) {
        case HASH:
          hasher_finalize(&ijob->as.hash_job.hasher, ijob->as.hash_job.hash,
                          sizeof(ijob->as.hash_job.hash));

          task_free(&worker, itask);

          print_hash_result(&worker, &ijob->as.hash_job);
          break;
        case PRINT:
          task_free(&worker, itask);
          break;
        default:
          assert(false && "unreachable");
          break;
        }
      }
    }

    task_io_submissions_flush(&worker);
    io_uring_cq_advance(&worker.ring, n);
  }

done:
  worker_free(&worker);
  return NULL;
}
