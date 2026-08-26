#include "ride.h"
#include "queue.h"
#include "ride.skel.h"
#include "worker.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ride_cli_args {
  char watch_path[100];
  char fingerprint_alg[10];
};

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

int ride_run(int argc, char *argv[]) {
  struct ride_cli_args args = {
      .watch_path = {0},
      .fingerprint_alg = "blake3"
  };

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
  struct event event;
  int bpf_queue_fd;
  int err;

  obj = ride_bpf__open();
  strncpy(obj->rodata->watch_path, args.watch_path, MAX_FILENAME_LEN); 
  obj->rodata->watch_path_len = strlen(args.watch_path);
  obj->rodata->userspace_pid = getpid();

  if ((err = ride_bpf__load(obj))) {
    fprintf(stderr, "bpf load error: %d\n", err);
    return EXIT_FAILURE;
  }

  bpf_queue_fd = bpf_object__find_map_fd_by_name(obj->obj, "queue");

  for (long i = 0; i < 10; i++) {
    pthread_t thread;
    pthread_create(&thread, NULL, &worker_run, (void *)i);
  }

  ride_bpf__attach(obj);

  while (1) {
    if (bpf_map_lookup_and_delete_elem(bpf_queue_fd, NULL, &event) == 0) {
      queue_add(&event);
    }
  }


}
