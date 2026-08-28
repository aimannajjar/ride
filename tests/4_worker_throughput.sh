#!/usr/bin/env bash

### End-to-end cold-cache throughput (long window)
## Same as test 1 but over a longer window: 8KB random payloads,
## caches dropped, every hash verified against b3sum.
## Goal: full end-to-end measurement with better statistical accuracy.
###

ID=4
DURATION=5
SKIP_CORRECTNESS=0
DUMMY_FILES_N=15000
EMPTY_FILES=0
bash ./0_worker_throughput.sh ${ID} ${DURATION} ${SKIP_CORRECTNESS} ${DUMMY_FILES_N} ${EMPTY_FILES}

