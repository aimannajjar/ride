#!/usr/bin/env bash

### Worker-pipeline throughput (quick iteration)
## Short empty-payload run with correctness skipped, intended for
## rapid before/after comparisons during development.
###

ID=3
DURATION=1
SKIP_CORRECTNESS=1
DUMMY_FILES_N=10000
EMPTY_FILES=1
TESTS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
bash "${TESTS_DIR}"/0_worker_throughput.sh ${ID} ${DURATION} ${SKIP_CORRECTNESS} ${DUMMY_FILES_N} ${EMPTY_FILES}

