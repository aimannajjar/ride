#!/usr/bin/env bash

### Run a perf record session under high load

ID=0
DURATION=5
SKIP_CORRECTNESS=1
PERF=2
DUMMY_FILES_N=25000
EMPTY_FILES=0

TESTS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
bash "${TESTS_DIR}"/0_worker_throughput.sh ${ID} ${DURATION} ${SKIP_CORRECTNESS} ${DUMMY_FILES_N} ${EMPTY_FILES} ${PERF}

sleep 1
sudo chown 1000:1000 perf.data* 

