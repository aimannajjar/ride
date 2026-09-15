#ifndef RIDE_H
#define RIDE_H

#include <stdbool.h>
#define MAX_FILENAME_LEN 128
#define MAX_THREADS 20
#define MAX_CONCURRENCY MAX_CONCURRENT_TASKS
#define DEFAULT_THREADS                                                        \
  (sysconf(_SC_NPROCESSORS_ONLN) < 1 ? 1 : sysconf(_SC_NPROCESSORS_ONLN) / 2)
#define DEFAULT_IO_CONCURRENCY 32


enum watch_path_type {
  WATCH_FILE,
  WATCH_DIRECRTORY,
};

struct event {
  char path[MAX_FILENAME_LEN];
};

struct verifier_config {
  bool requires_background_job;
};

int ride_run(int argc, char *argv[]);

#endif // RIDE_H
