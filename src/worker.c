#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#define _GNU_SOURCE
#include "client.h"
#include "hasher.h"
#include "log.h"
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
#define DEFAULT_TIMEOUT INT_MAX

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

[[maybe_unused]]
static inline char *job_type_name(enum job_type t) {
  static char *types[] = {"HASH", "PRINT", "VERIFY", "GENERIC"};
  return types[t];
}

enum task_state {
  IDLE,
  RUNNING,
};

// encodes parameters specific to hash jobs
struct job_hash {
  unsigned char hash[HASH_LEN];
  char path[MAX_FILENAME_LEN];
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
  // io_uring / event loop data
  struct io_uring ring;
  int epollfd;
  int queue_efd; // eventfd for main thread producer notifies (passed as warg)

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

#ifdef DEBUG
#define LIST_INTEGRITY_CHECK(worker, skip, verbose)                            \
  (list_integrity_check(worker, skip, verbose))
#else
#define LIST_INTEGRITY_CHECK(worker, skip, verbose)
#endif

[[maybe_unused]]
void static inline list_integrity_check(const struct worker *worker,
                                        ssize_t skip_slot_id, bool verbose) {
#define COND_LOG(...)                                                          \
  {                                                                            \
    if (verbose)                                                               \
      log_trace(__VA_ARGS__);                                                  \
  }

#define ASSERT(P, ...)                                                         \
  {                                                                            \
    if (!(P))                                                                  \
      log_error(__VA_ARGS__);                                                  \
    assert(P);                                                                 \
  }

  struct task *task;
  struct job_slot *slot;
  size_t seen_ids[JOB_QUEUE_DEPTH] = {0};
  ssize_t i;

  log_trace("checking lists integrity");
  COND_LOG("------------ tasks ------------- ");
  for (i = 0; i < worker->nr_tasks; i++) {
    task = &worker->tasks[i];
    if (task->state == RUNNING) {
      COND_LOG("task %ld, slot id: %zd, type: %s", task->id, task->job_slot->id,
               job_type_name(task->job_slot->job.type));

      switch (task->job_slot->job.type) {
      case HASH:
        COND_LOG("  path: %s", task->job_slot->job.as.hash_job.path);
        break;
      case PRINT:
        COND_LOG("  buffer: %.*s", task->job_slot->job.as.print_job.len - 1,
                 task->job_slot->job.as.print_job.msg);
        break;
      default:
        break;
      }
      seen_ids[task->job_slot->id] = 1;
    }
  }

  COND_LOG("------------ queue ------------- ");
  slot = worker->queued_head;
  while (slot) {
    ASSERT(!seen_ids[slot->id], "slot_id %zd seen in task and queue list",
           slot->id);
    COND_LOG("slot id %zd (type=%s)", slot->id, job_type_name(slot->job.type));

    switch (slot->job.type) {
    case HASH:
      COND_LOG("  path: %s", slot->job.as.hash_job.path);
      break;
    case PRINT:
      COND_LOG("  buffer: %.*s", slot->job.as.print_job.len - 1,
               slot->job.as.print_job.msg);
      break;
    default:
      break;
    }

    seen_ids[slot->id] = 1;
    slot = slot->next;
  }

  COND_LOG("----------- free list ----------");
  slot = worker->free_slots;
  while (slot) {
#ifdef LOG_TRACE
    if (verbose)
      printf("| %zd | -> ", slot->id);
#endif

    // it's possible for slot to briefly appear in task and free list, this
    // happens when the job is returned to free_list but task still hasn't
    // been freed yet, see task_free and return_job
    if (skip_slot_id == slot->id) {
      seen_ids[slot->id] = 1;
      slot = slot->next;
      continue;
    }

    ASSERT(!seen_ids[slot->id],
           "slot_id %zd seen in free list and some other list", slot->id);
    seen_ids[slot->id] = 1;
    slot = slot->next;
  }

#ifdef LOG_TRACE
  if (verbose)
    printf("\n");
#endif
  COND_LOG("--------------------------------");

  for (i = 0; i < JOB_QUEUE_DEPTH; i++) {
    // it's possible for a job slot to float, i.e not exist in any list
    // this happens brielfy when a slot has been dequeued and lists checked
    // before the slot has been scheduled in the tasks list (see task_take)
    if (skip_slot_id == i)
      continue;

    ASSERT(seen_ids[i], "slot_id %zd not seen in any list", i);
  }
  log_trace("lists integrity validated");
#undef COND_LOG
}

// Adds a job to queue list
// if job_slot is NULL, it will pick a job slot from free list
// if job_slot is specified, it should have been acquired previously
// from enqueue_job
static struct job_slot *enqueue_job(struct worker *worker,
                                    struct job_slot *job_slot) {
  log_trace("enqueing a job");

  LIST_INTEGRITY_CHECK(worker, (job_slot ? job_slot->id : -1), false);

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

  log_trace("enqueued job_slot slot_id=%zd (%p) job_id=%zd (%p)", job_slot->id,
            job_slot, job_slot->job.id, &job_slot->job);

  LIST_INTEGRITY_CHECK(worker, -1, false);
  return job_slot;
}

// Pops first element from queue list (e.g. when taking a task)
// Note, the caller must return it to free list by calling return job
// or alternatively call requeue to add it back to queue list
static struct job_slot *dequeue_job(struct worker *worker) {
  struct job_slot *job_slot;
  log_trace("dequing first job from queue");

  LIST_INTEGRITY_CHECK(worker, -1, false);

  // take first free queued job, and move head to next one
  if (!worker->queued_head)
    return NULL; // nothing is queued

  // move head
  job_slot = worker->queued_head;
  worker->queued_head = worker->queued_head->next;
  if (worker->queued_head)
    worker->queued_head->prev = NULL;

  log_trace("dequeued job slot_id=%zd (%p) id=%zd (%p)", job_slot->id, job_slot,
            job_slot->job.id, &job_slot->job);

  LIST_INTEGRITY_CHECK(worker, job_slot->job.id, false);
  return job_slot;
}

// returns a job slot to free list
static void return_job(struct worker *worker, struct job_slot *job_slot) {
  log_trace("returning job slot_id=%zd (%p) id=%zd (%p)", job_slot->id,
            job_slot, job_slot->job.id, &job_slot->job);

  LIST_INTEGRITY_CHECK(worker, -1, true);
  job_slot->next = worker->free_slots;
  job_slot->prev = NULL; // free list is singly linked list
  worker->free_slots = job_slot;
  LIST_INTEGRITY_CHECK(worker, job_slot->job.id, false);
}

// This has the same effect as calling enqueue_job and passing job_slot
static void requeue_job(struct worker *worker, struct job_slot *job_slot) {
  log_trace("requeuing job slot_id=%zd (%p) id=%zd (%p)", job_slot->id,
            job_slot, job_slot->job.id, &job_slot->job);

  // skip job slot id check because slot is now floating before it's requeued
  LIST_INTEGRITY_CHECK(worker, job_slot->id, false);
  enqueue_job(worker, job_slot);
  LIST_INTEGRITY_CHECK(worker, -1, false);
}

static void worker_setup(struct worker *worker,
                         const struct worker_args *wargs) {
  ssize_t i;
  struct epoll_event ev;
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
  worker->slots[JOB_QUEUE_DEPTH - 1].job.id = JOB_QUEUE_DEPTH - 1;
  for (i = JOB_QUEUE_DEPTH - 2; i >= 0; i--) {
    worker->slots[i].next = &worker->slots[i + 1];
    worker->slots[i].id = i;
    worker->slots[i].job.id = i;
  }
  worker->free_slots = worker->slots;
  worker->queued_head = NULL;
  worker->queued_tail = NULL;

  // setup epoll
  worker->epollfd = epoll_create1(0);
  worker->queue_efd = wargs->queue_eventfd;
  if (worker->epollfd == -1) {
    perror("epoll_create1");
    exit(EXIT_FAILURE);
  }

  ev.events = EPOLLIN | EPOLLET | EPOLLEXCLUSIVE;
  ev.data.fd = wargs->queue_eventfd;
  if (epoll_ctl(worker->epollfd, EPOLL_CTL_ADD, wargs->queue_eventfd, &ev)) {
    perror("epoll_ctl");
    exit(EXIT_FAILURE);
  };

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

  log_trace("taking a new task from queue");
  LIST_INTEGRITY_CHECK(worker, -1, false);

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

  log_trace("took task tid = %zd", tid);
  LIST_INTEGRITY_CHECK(worker, -1, true);

  return task;
}

static void task_hash_prep_submit(struct worker *worker, struct task *task) {
  struct io_uring_sqe *sqe;
  struct job *job = &task->job_slot->job;
  struct job_hash *hjob = &job->as.hash_job;
  sqe = io_uring_get_sqe(&worker->ring);
  io_uring_prep_read(sqe, hjob->fd, worker->buffers[task->id],
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

void enqueue_job_verify(struct worker *worker, const struct job_hash *hjob) {
  struct job_slot *job_slot;
  struct job *job;

  log_trace("enqueuing hash verify job");
  job_slot = enqueue_job(worker, NULL);
  job = &job_slot->job;

  // populate job
  memcpy(job->as.verify_job.param.hash, hjob->hash,
         sizeof(job->as.hash_job.hash));

  memcpy(job->as.verify_job.param.path, hjob->path,
         sizeof(job->as.verify_job.param.path));

  job->as.verify_job.retries = 0;
  job->type = VERIFY;
}

static void enqueue_job_hash(struct worker *worker, const struct event *event,
                             int fd) {
  struct job_slot *job_slot;
  struct job *job;

  log_trace("enqueuing hash job for path=%s len=%ld", event->path,
            event->path_len);
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
static void enqueue_job_print(struct worker *worker,
                              const char path[MAX_FILENAME_LEN], size_t len,
                              const char msg[len]) {
  struct job_slot *job_slot;
  struct job *job;

  log_trace("enqueuing hash print results job");
  job_slot = enqueue_job(worker, NULL);
  job = &job_slot->job;

  // populate job
  job->type = PRINT;
  assert(len < sizeof(job->as.print_job.msg) &&
         "Bug: async output message too large");

  memcpy(job->as.print_job.msg, msg, sizeof(job->as.print_job.msg));
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

// task_free: frees both the task executor and job_slot being executed by it
// - keep_job: set to true if you don't want to free the job_slot
//      useful when you plan to reschedule the job and want state preserved
static void task_free(struct worker *worker, struct task *task, bool keep_job) {
  struct job_slot *job_slot;
  struct job *job;
  [[maybe_unused]] size_t skip_check;

  log_trace("freeing task %ld, slot id: %zd, type: %s", task->id,
            task->job_slot->id, job_type_name(task->job_slot->job.type));

  LIST_INTEGRITY_CHECK(worker, -1, false);
  job_slot = task->job_slot;
  job = &task->job_slot->job;
  skip_check = job_slot->id;
  if (!keep_job) {
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
      // TODO
      break;
    default:
      assert(false && "not implemented");
      break;
    }

    // move slot back from queued to free list
    return_job(worker, job_slot);

    skip_check = -1; // no need to skip check, slot should appea in free list
  }

  // free up task executor and push its id to stack
  task->job_slot = NULL;
  task->state = IDLE;
  worker->free_tids[--worker->tsp] = task->id;
  LIST_INTEGRITY_CHECK(worker, skip_check, false);
}

static void task_reschedule(struct worker *worker, struct task *task) {
  struct job_slot *job_slot;
  job_slot = task->job_slot;
  log_trace("rescheduling task %ld, slot id: %zd, type: %s", task->id,
            task->job_slot->id, job_type_name(task->job_slot->job.type));

  task_free(worker, task, true);

  // put the job back on queue list to reschedule it
  requeue_job(worker, job_slot);
}

void static inline print_hash_result(struct worker *worker,
                                     const struct job_hash *htask) {
  log_trace("printing hash results for %s", htask->path);
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

  enqueue_job_print(worker, htask->path, mlen, debug);
}

// Returns true if there are not tasks in execution and also no jobs in our
// job queue. This excludes verifier background job which is always running
// This can be used by callers to block until jobs are available to enqueue
static bool need_work(const struct worker *worker) {
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
  struct epoll_event epevents[1];
  struct epoll_event verifier_ev;
  uint64_t timeout;
  uint64_t verifier_fd;
  int vbg_flags;

  wargs = (struct worker_args *)args;
  worker_setup(&worker, wargs);
  free(wargs);
  wargs = NULL;

  // event loop
  timeout = 1;
  verifier_fd = 0;
  vbg_flags = 0;
  while (!atomic_load_explicit(&quit, memory_order_acquire)) {
    int ret, fd;
    uint64_t ev_sem;
    struct event event;
    bool new_file_event = false;

    if (need_work(&worker)) {
      log_trace("polling");
      ret = epoll_wait(worker.epollfd, epevents, 1, timeout);
      // we have no tasks, poll
      if (ret && epevents[0].data.fd == worker.queue_efd) {
        if (quit)
          goto done;
        new_file_event = !queue_consume_try(&event);
        if (read(epevents[0].data.fd, &ev_sem, sizeof ev_sem) !=
            sizeof(ev_sem)) {
          perror("read");
          exit(1);
        }
        vbg_flags = 0;
      } else if (ret && epevents[0].data.fd == verifier_fd) {
        vbg_flags = VBG_RECV;
      } else {
        vbg_flags = VBG_TIMEOUT;
      }
    } else {
      new_file_event = !queue_consume_try(&event);
    }

    if (new_file_event) {
      // we've consumed new task, schedule it in async loop
      if ((fd = open(event.path, O_RDONLY)) < 0) {
        log_error("Error opening: %s: %s", event.path, strerror(errno));
        continue;
      }

      enqueue_job_hash(&worker, &event, fd);
    }

    // Take a task
    struct task *task;
    struct job *job;
    int new_sock_fd;
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
      case VERIFY:
        log_debug("send verify request: %s", job->as.verify_job.param.path);
        if (client_verify_request(&worker.verifier, &job->as.verify_job)) {
          log_debug("rescheduling task");
          job->as.verify_job.retries++;

          if (job->as.verify_job.retries > 3) {
            log_error("maximum retries reached, gave up on verifying: file=%s",
                      job->as.verify_job.param.path);
            // TODO: accounting and log to disk
            task_free(&worker, task, false);
          } else {
            task_reschedule(&worker, task);
          }
        } else {
          task_free(&worker, task, false);
        }
        break;
      case GENERIC:
        assert(job->as.generic_job.callback &&
               "GENERIC job callback undefined");
        // ideally we want minimum timeout of all jobs but currently we only
        // have one job that publishes a timeout (verifier bg job)
        if (!(timeout = job->as.generic_job.callback(
                  job->as.generic_job.callback_arg, vbg_flags, &new_sock_fd))) {
          // when generic job returns zero, it means it completed
          timeout = DEFAULT_TIMEOUT;
          task_free(&worker, task, false);
        } else {
          // non zero return, means task want to be rescheduled with returned
          // timeout
          task_reschedule(&worker, task);
        }
        if (new_sock_fd != verifier_fd) {
          // clean up old fd
          if (verifier_fd) {
            verifier_ev.events = EPOLLIN;
            verifier_ev.data.fd = verifier_fd;
            epoll_ctl(worker.epollfd, EPOLL_CTL_DEL, verifier_fd, &verifier_ev);
            log_trace("remove old verifier fd: %d from epoll set", verifier_fd);
          }

          if (new_sock_fd) {
            verifier_ev.events = EPOLLIN;
            verifier_ev.data.fd = new_sock_fd;
            log_trace("add verifier fd: %d to epoll set", new_sock_fd);
            epoll_ctl(worker.epollfd, EPOLL_CTL_ADD, new_sock_fd, &verifier_ev);
            verifier_fd = new_sock_fd;
          }
        }
        break;
      default:
        assert(false && "not implemented");
        break;
      }
    }

    struct io_uring_cqe *cqes[worker.nr_tasks];
    size_t i, n;
    n = io_uring_peek_batch_cqe(&worker.ring, cqes, worker.nr_tasks);

    // TODO: integrate epoll into io_uring and poll via io_uring, or use
    // io_uring's eventfd capability to notify epoll_wait
    while (!n && all_tasks_in_io_uring(&worker)) {
      _mm_pause();
      n = io_uring_peek_batch_cqe(&worker.ring, cqes, worker.nr_tasks);
    }

    // process completions
    for (i = 0; i < n; i++) {
      task = (struct task *)io_uring_cqe_get_data(cqes[i]);
      job = &task->job_slot->job;
      if (cqes[i]->res < 0) {
        log_error("io_uring error task type %d", job->id);
        switch (job->type) {
        case HASH:
          log_error("(slot %zu) Error async read: %s: %s\n", task->id,
                    job->as.hash_job.path, strerror(-cqes[i]->res));
          break;
        case PRINT:
          break;
        default:
          // handle any future tasks that are submitted through io uring here
          assert(false && "unreachable");
          break;
        }
        task_free(&worker, task, false);
        continue;
      } else if (cqes[i]->res > 0) {
        switch (job->type) {
        case HASH:
          hasher_update(&job->as.hash_job.hasher, cqes[i]->res,
                        worker.buffers[task->id]);

          // read next chunk
          job->as.hash_job.offset += cqes[i]->res;
          task_hash_prep_submit(&worker, task);
          break;
        case PRINT:
          job->as.print_job.offset += cqes[i]->res;
          task_print_prep_submit(&worker, task);
          break;
        default:
          assert(false && "unreachable");
          break;
        }
      } else {
        // final completion
        switch (job->type) {
        case HASH:
          hasher_finalize(&job->as.hash_job.hasher, job->as.hash_job.hash,
                          sizeof(job->as.hash_job.hash));

          print_hash_result(&worker, &job->as.hash_job);
          if (worker.verify)
            enqueue_job_verify(&worker, &job->as.hash_job);

          // TODO: consider keeping around for zero-copy for the above
          task_free(&worker, task, false);
          break;
        case PRINT:
          task_free(&worker, task, false);
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
