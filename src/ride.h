#ifndef RIDE_H
#define RIDE_H

#define MAX_FILENAME_LEN 256
#define BPF_RING_BUF_SIZE 4096

struct event {
  char path[MAX_FILENAME_LEN];
};

int ride_run(int argc, char *argv[]);

#endif // RIDE_H
