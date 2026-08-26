#include "watcher.h"
#include <stdlib.h>
#include <pthread.h>

struct rides_watcher {
  pthread_t thread;
};



rides_watcher_t watcher_init() {
  struct rides_watcher *r = malloc(sizeof *r);
  if (!r) {
    perror("malloc");
    return NULL;
  }

  return 0;
}


int watcher_close(rides_watcher_t w) {

  return 0;
}
