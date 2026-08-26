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
  char filename[100];
  char db[32];
  char fingerprint_alg[10];
};

/** most args are not actually used
 ** except for filename
 **/
int parse_env(struct ride_cli_args *out, int argc, char *argv[]) {
  // -d  database file = defaults to ride.db
  // -f [FP_ALG] = use fingerprinting, specify algorithm or defaults to SHA-512
  const char optstring[] = ":d:f::";
  while (1) {

    int ch = getopt(argc, argv, optstring);
    if (-1 == ch)
      break;

    switch (ch) {
    case 'd':
      strcpy(out->db, optarg);
      break;
    case 'f':
      if (0 != optarg) {
        strncpy(out->fingerprint_alg, optarg, sizeof(out->fingerprint_alg));
        out->fingerprint_alg[sizeof out->fingerprint_alg - 1] = '\0';
      } else {
        strcpy(out->fingerprint_alg, "SHA-512");
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
    printf("Missing required argument: path\n");
    return EXIT_FAILURE;
  }

  strncpy(out->filename, argv[optind], sizeof out->filename);
  out->filename[sizeof out->filename - 1] = '\0';
  return 0;
}

int ride_run(int argc, char *argv[]) {
  struct ride_bpf *obj;
  struct event event;
  int bpf_queue_fd;

  obj = ride_bpf__open_and_load();
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

  // struct ride_cli_args args = {
  //     .filename = {0}, .db = {0}, .fingerprint_alg = {0}};
  //
  // if (parse_env(&args, argc, argv)) {
  //   return EXIT_FAILURE;
  // }
  //
  // printf("Starting RIDES with filename=%s, db=%s, fp=%s\n", args.filename,
  //        args.db, args.fingerprint_alg);
  //
  // FILE *fp = fopen(args.filename, "rb");
  // if (fp == NULL) {
  //   perror("open");
  //   return EXIT_FAILURE;
  // }
  //
  // unsigned char filehash[HASH_LEN];
  // hash(filehash, fp);
  //
  // for (size_t i = 0; i < HASH_LEN; i++) {
  //   printf("%02x", filehash[i]);
  // }
  // printf("\n");
  //
  // fclose(fp);
}
