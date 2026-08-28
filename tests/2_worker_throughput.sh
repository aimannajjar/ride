#!/usr/bin/env bash

### Worker-pipeline throughput (long window)
## Same open-loop load, longer window, but over empty files.
## - Empty payloads minimize BLAKE3 cost, isolating event-drain
##   (worker parallelism) throughput
## - Also keeps a long window practical
## Correctness is skipped here; it is established by the verified runs.
###

ID=2
DURATION=5
SKIP_CORRECTNESS=1
DUMMY_FILES_N=20000
EMPTY_FILES=1
bash ./0_worker_throughput.sh ${ID} ${DURATION} ${SKIP_CORRECTNESS} ${DUMMY_FILES_N} ${EMPTY_FILES}

