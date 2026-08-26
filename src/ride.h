#ifndef RIDES_H
#define RIDES_H

struct event {
  char path[256];
};

int ride_run(int argc, char *argv[]);

#endif
