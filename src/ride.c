#include "ride.h"
#include "queue.h"
#include "ride.skel.h"
#include "worker.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <fcntl.h>
#include <jemalloc/jemalloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct ride_cli_args {
  char watch_path[MAX_FILENAME_LEN];
  size_t watch_path_len;
  enum watch_path_type watch_path_type;
  char fingerprint_alg[10];
  size_t threads;
  size_t io_concurrency;
};

atomic_int quit = 0;
extern pthread_cond_t queue_cond; // queue.c

/** most args are not actually used
 ** except for filename
 **/
int parse_env(struct ride_cli_args *out, int argc, char *argv[]) {
  // -f [FP_ALG] = use fingerprinting, specify algorithm or defaults to BLAKE3
  //               currenlty only BLAKE3 is supported anyway
  // -t [NUM_THREADS] how many worker threads
  // -c [IO_CONCURRENCY] how many concurrent IO tasks *per* worker thread
  const char optstring[] = ":f::t:c:";
  while (1) {
    int ch = getopt(argc, argv, optstring);
    if (-1 == ch)
      break;

    switch (ch) {
    case 'f':
      if (0 != optarg) {
        strncpy(out->fingerprint_alg, optarg, sizeof(out->fingerprint_alg));
        out->fingerprint_alg[sizeof out->fingerprint_alg - 1] = '\0';
      } else {
        strcpy(out->fingerprint_alg, "BLAKE3");
      }
      break;
    case 't':
      out->threads = strtoll(optarg, NULL, 0);
      if (!out->threads || out->threads > MAX_THREADS) {
        fprintf(stderr, "Invalid threads argument\n");
        return EXIT_FAILURE;
      }
      break;
    case 'c':
      out->io_concurrency = strtoll(optarg, NULL, 0);
      if (!out->io_concurrency || out->io_concurrency > MAX_CONCURRENCY) {
        fprintf(stderr, "Invalid concurrency argument\n");
        return EXIT_FAILURE;
      }
      break;
    case ':':
      printf("Missing required argument for option '%c'\n", optopt);
      return 1;
      break;
    case '?':
      printf("Invalid option: '%c'\n", optopt);
      return EXIT_FAILURE;
      break;
    }
  }

  if (optind == argc) {
    printf("Missing required argument: watch_path\n");
    return EXIT_FAILURE;
  }

  strncpy(out->watch_path, argv[optind], sizeof out->watch_path);
  out->watch_path[sizeof out->watch_path - 1] = '\0';
  return 0;
}

static void ride_sig_handler(int signal) {
  printf("Shutdown signal received, exiting.\n");
  atomic_store_explicit(&quit, true, memory_order_release);
  pthread_cond_broadcast(&queue_cond);
  sleep(1);
#ifdef USERSPACE_TRACE
  malloc_stats_print(NULL, NULL, NULL);
#endif
}

int ride_ringbuf_handle(void *ctx, void *data, size_t sz) {
  queue_add((struct event *)data);
  return 0;
}

int ride_stat(struct ride_cli_args *ride) {
  struct stat sb;
  if (stat(ride->watch_path, &sb)) {
    perror("stat");
    return 1;
  }

  if (S_ISDIR(sb.st_mode)) {
    ride->watch_path_type = WATCH_DIRECRTORY;
  } else if (S_ISREG(sb.st_mode)) {
    ride->watch_path_type = WATCH_FILE;
  } else {
    fprintf(
        stderr,
        "Unsupported watch path (mode=%d). Currently only supporting file or "
        "directories\n",
        sb.st_mode);
    return 1;
  }

  char *abspath;
  size_t pathlen;
  abspath = realpath(ride->watch_path, NULL);
  if (!abspath) {
    perror("realpath");
    return 1;
  }

  if ((pathlen = strlen(abspath)) > sizeof(ride->watch_path) - 1) {
    fprintf(stderr, "canonical path is too large: %s\n", abspath);
    free(abspath);
    return 1;
  } else if (ride->watch_path_type == WATCH_DIRECRTORY &&
             pathlen > sizeof(ride->watch_path) - 2) {
    // a trailing / will be appeneded to directories, size max is reduced by 1
    fprintf(stderr,
            "Watch path too large, please note for directories the max is %d\n",
            MAX_FILENAME_LEN - 1);
  }

  strncpy(ride->watch_path, abspath, sizeof(ride->watch_path));
  ride->watch_path[sizeof(ride->watch_path) - 1] = '\0';

  if (ride->watch_path_type == WATCH_DIRECRTORY) {
    // append trailing / for directories
    ride->watch_path[pathlen] = '/';
    ride->watch_path_len = pathlen + 1;

  } else {
    ride->watch_path_len = pathlen;
  }
  free(abspath);

  if (ride->watch_path_type == WATCH_DIRECRTORY &&
      ride->watch_path_len > MAX_FILENAME_LEN * 0.90) {
    fprintf(stderr,
            "Warning: Watched directory legnth is very large, there is "
            "a max path of %d. If total path of monitored files exceed "
            "it, they will be silently dropped\n",
            MAX_FILENAME_LEN);
  }

  return 0;
}

int ride_run(int argc, char *argv[]) {
  struct ride_cli_args args = {.watch_path = {0},
                               .fingerprint_alg = "BLAKE3",
                               .threads = DEFAULT_THREADS,
                               .io_concurrency = DEFAULT_IO_CONCURRENCY};

  if (parse_env(&args, argc, argv)) {
    return EXIT_FAILURE;
  }

  if (ride_stat(&args)) {
    return EXIT_FAILURE;
  }

  printf("Starting RIDE with watch_path=%s, fp=%s, threads=%ld, "
         "io_concurrency=%ld\n",
         args.watch_path, args.fingerprint_alg, args.threads,
         args.io_concurrency);

  struct ride_bpf *obj;
  struct ring_buffer *rb;
  int ring_fd;
  int err;

  queue_init();

  obj = ride_bpf__open();
  strncpy(obj->rodata->watch_path, args.watch_path, MAX_FILENAME_LEN);
  obj->rodata->watch_path_len = args.watch_path_len;
  obj->rodata->watch_path_type = args.watch_path_type;
  obj->rodata->userspace_pid = getpid();

  if ((err = ride_bpf__load(obj))) {
    fprintf(stderr, "bpf load error: %d\n", err);
    return EXIT_FAILURE;
  }

  ring_fd = bpf_map__fd(obj->maps.rb);
  rb = ring_buffer__new(ring_fd, ride_ringbuf_handle, NULL, NULL);

  for (long i = 0; i < args.threads; i++) {
    pthread_t thread;
    struct worker_args *wargs = malloc(sizeof(struct worker_args));
    wargs->id = i;
    wargs->io_concurrency = args.io_concurrency;
    pthread_create(&thread, NULL, &worker_run, (void *)wargs);
  }

  if ((err = ride_bpf__attach(obj))) {
    fprintf(stderr, "bpf attach error: %d\n", err);
    ring_buffer__free(rb);
    return EXIT_FAILURE;
  }

  signal(SIGINT, ride_sig_handler);
  signal(SIGTERM, ride_sig_handler);
  signal(SIGHUP, ride_sig_handler);
  while (!quit) {
    err = ring_buffer__poll(rb, 100);
    if (err < 0 && err != -EINTR) {
      perror("ringbuf poll error");
      break;
    }
  }

  ring_buffer__free(rb);
  if (err)
    return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
