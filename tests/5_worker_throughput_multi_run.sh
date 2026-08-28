#!/usr/bin/env bash

### End-to-end cold-cache throughput (long window, skip correctness)
## Same as test 4 but skipping correctness
## Goal: full end-to-end measurement with better statistical accuracy.
##       but with assumption that hashes are correct (established by test #4)
###

ID=5
DURATION=5
SKIP_CORRECTNESS=1
DUMMY_FILES_N=25000
EMPTY_FILES=0

for _ in {1..5}; do
  bash ./0_worker_throughput.sh ${ID} ${DURATION} ${SKIP_CORRECTNESS} ${DUMMY_FILES_N} ${EMPTY_FILES}
  sleep 10;
done

