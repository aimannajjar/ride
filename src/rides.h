#ifndef RIDES_H
#define RIDES_H

struct event {
  char path[256];
};

int rides_run(int argc, char *argv[]);

#endif
