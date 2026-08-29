#include "ride.h"
#include <stdio.h>

// Real-time Intrusion Detection Events
int main(int argc, char *argv[]) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  return ride_run(argc, argv);
}

