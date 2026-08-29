#!/usr/bin/env bash

### End-to-end cold-cache throughput (short window)
## Open-loop load for 250ms over 8KB random payloads, caches dropped.
## Verifies every hash against b3sum, then measures throughput.
## Slower, but provides full end-to-end validation.
###

ID=1
DURATION=0.25
SKIP_CORRECTNESS=0
DUMMY_FILES_N=4000
EMPTY_FILES=0
TESTS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
bash "${TESTS_DIR}"/0_worker_throughput.sh ${ID} ${DURATION} ${SKIP_CORRECTNESS} ${DUMMY_FILES_N} ${EMPTY_FILES}

