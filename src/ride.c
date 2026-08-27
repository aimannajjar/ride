#include "ride.h"
#include "queue.h"
#include "ride.skel.h"
#include "worker.h"
#include <asm-generic/errno-base.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ride_cli_args {
  char watch_path[100];
  char fingerprint_alg[10];
};

static volatile bool quit = false;

/** most args are not actually used
 ** except for filename
 **/
int parse_env(struct ride_cli_args *out, int argc, char *argv[]) {
  // -f [FP_ALG] = use fingerprinting, specify algorithm or defaults to BLAKE3
  // currenlty only BLAKE3 is supported anyway
  const char optstring[] = ":h::";
  while (1) {

    int ch = getopt(argc, argv, optstring);
    if (-1 == ch)
      break;

    switch (ch) {
    case 'h':
      if (0 != optarg) {
        strncpy(out->fingerprint_alg, optarg, sizeof(out->fingerprint_alg));
        out->fingerprint_alg[sizeof out->fingerprint_alg - 1] = '\0';
      } else {
        strcpy(out->fingerprint_alg, "BLAKE3");
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

static void sig_handler(int signal) { quit = true; }

int ride_ringbuf_handle(void *ctx, void *data, size_t sz) {
  queue_add((struct event *)data);
  return 0;
}


int ride_run(int argc, char *argv[]) {
  struct ride_cli_args args = {.watch_path = {0}, .fingerprint_alg = "blake3"};

  if (parse_env(&args, argc, argv)) {
    return EXIT_FAILURE;
  }

  if (strlen(args.watch_path) > MAX_FILENAME_LEN) {
    printf("watch_path too large\n");
    return EXIT_FAILURE;
  }

  printf("Starting RIDE with watch_path=%s, fp=%s\n", args.watch_path,
         args.fingerprint_alg);

  struct ride_bpf *obj;
  struct ring_buffer *rb;
  struct event event;
  int ring_fd;
  int err;

  obj = ride_bpf__open();
  strncpy(obj->rodata->watch_path, args.watch_path, MAX_FILENAME_LEN);
  obj->rodata->watch_path_len = strlen(args.watch_path);
  obj->rodata->userspace_pid = getpid();

  if ((err = ride_bpf__load(obj))) {
    fprintf(stderr, "bpf load error: %d\n", err);
    return EXIT_FAILURE;
  }

  ring_fd = bpf_map__fd(obj->maps.rb);
  rb = ring_buffer__new(ring_fd, ride_ringbuf_handle, NULL, NULL);

  for (long i = 0; i < 10; i++) {
    pthread_t thread;
    pthread_create(&thread, NULL, &worker_run, (void *)i);
  }

  ride_bpf__attach(obj);

  signal(SIGINT, sig_handler);
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
