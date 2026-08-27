#ifndef RIDE_H
#define RIDE_H

#define USERSPACE_DEBUG
#define MAX_FILENAME_LEN 256

struct event {
  char path[MAX_FILENAME_LEN];
};

int ride_run(int argc, char *argv[]);

#endif // RIDE_H
