#ifndef WATCHER_H
#define WATCHER_H

#include "hasher.h"

typedef struct rides_watcher *rides_watcher_t;

struct rides_watch {
  FILE *fp;
  char expected_hash[HASH_LEN];
};

#endif
