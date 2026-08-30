#!/usr/bin/env bash

# ---------------------------------------------------------------------------
# Open-loop throughput benchmark (saturation test)
# Generates file-open events at maximum rate to trigger the LSM file_open
# hook, without waiting for RIDE to process previous events, and measures
# completed hashes over a fixed window.
#
# Parameters:
# 1. Duration: length of the measurement window
# 2. Skip Correctness: skip hash verification against b3sum
#    - useful for long windows where verification is impractical
#    - only valid once correctness is established by a verified run
# 3. Workload File Count: distinct files the generator cycles through
#    - must be large enough to keep RIDE busy for the whole window
#    - the generator wraps to a second pass once all files are opened;
#      a wrap only distorts results if RIDE finished the first pass,
#      since repeat reads are then served from the page cache
#    - the generator warns on the first wrap
# 4. Empty Files: use empty files instead of 8KB random payloads
#    - minimizes BLAKE3 cost to isolate worker-pipeline throughput
##

set -euo pipefail
ulimit -n 65535

cleanup() {
  sudo kill "$PID" 2>/dev/null || true
  kill "$WORKLOAD_PID" 2>/dev/null || true
}

ID=${1:-1}
DURATION=${2:-"0.25"}
MS=$(echo "scale=2; $DURATION * 1000" | bc)
SKIP_CORRECTNESS=${3:-0}
DUMMY_FILES_N=${4:-2000}
EMPTY_FILES=${5:-0}
USE_PRE_EXISITNG_DUMMIES=1

RIDE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RIDE="${RIDE_ROOT}/build/ride"
TMP="${RIDE_ROOT}/tests/tmp"
if [ ! -f "$RIDE" ]; then
  echo "./build/ride was not found, did you build the project?"
  exit 1
fi

if (( ! USE_PRE_EXISITNG_DUMMIES )); then
    rm -rf "$TMP" && mkdir "$TMP"
else
    echo "WARNING: Using existing payloads in ${TMP}/dummy0..${DUMMY_FILES_N}, " \
         "if files don't exist results will be wrong " \
         "(unset USE_PRE_EXISITNG_DUMMIES to re-generate)"
    rm "$TMP"/output.txt
fi

sudo -v
touch "$TMP"/output.txt

echo "------- Open-loop throughput test ${ID}  -------"
echo 
echo "Test Paramters"
echo ---------------
printf "DURATION:\t\t%s ms\n" "$MS"
printf "SKIP CORRECTNESS:\t%s\n" "${SKIP_CORRECTNESS}"
printf "EMPTY_FILES:\t\t%s\n" "${EMPTY_FILES}"
printf "DUMMY_FILES_N:\t\t%s\n" "${DUMMY_FILES_N}"
echo ----------------
echo
if (( ! EMPTY_FILES && ! USE_PRE_EXISITNG_DUMMIES )); then
  echo "> Generate ${DUMMY_FILES_N} 8KB files"
  for ((i=0;i<DUMMY_FILES_N;i++)); do
    dd if=/dev/urandom of="${TMP}/dummy${i}" bs=8192 count=1 &>/dev/null
  done
  echo "> Flush and drop caches"
  sudo sync
fi

echo "> Drop caches"
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

echo "> Start RIDE"
trap cleanup EXIT
(sudo "$RIDE" "$TMP" -t 4 -c 32) > "${TMP}/output.txt" &
sleep 0.75
PID=$(pgrep "ride")

echo "> Simulate highly concurrent reads in background"
bash "${RIDE_ROOT}/tests/offered_load.sh" "$DUMMY_FILES_N" &
WORKLOAD_PID=$!

echo "> Start measuring"
start=$EPOCHREALTIME
sleep "$DURATION"
finish=$EPOCHREALTIME
echo "> Done!"
sudo pkill "$PID" || true

if (! kill $WORKLOAD_PID); then
  echo "! ERROR: Workload finished earlier than experiment end";
  exit 1;
fi

elapsed=$(awk -v s="$start" -v e="$finish" 'BEGIN {printf "%.3f", e - s }')

if (( ! SKIP_CORRECTNESS )); then
  echo "> Test correctness against b3sum"
  fail_count=0
  pass_count=0
  { read -r; while read -r line; do
    if ! file=$(echo "$line" | jq -r .file); then 
      echo "! Skipping invalid JSON '$line'"
      continue; 
    fi
    
    if ! hash=$(echo "$line" | jq -r .hash); then 
      echo "! Skipping invalid JSON '$line'"
      continue; 
    fi
  
    expected=$(b3sum --no-names "$file")
    if [[ "$expected" != "$hash" ]]; then
      fail_count=$((fail_count+1))
      echo "! ERROR: $file; expected=$expected got=$hash"
    else 
      pass_count=$((pass_count+1))
    fi
  
  done } < "${TMP}/output.txt"
else
  echo "! Skipping correctness per test parameters, assume all reads correct"
  pass_count=$(tail -n +2 "$TMP"/output.txt | wc -l)
  pass_count=$((pass_count-1))  # last process was probably interrupted
fi;

throughput=$(echo "scale=0; $pass_count/$elapsed" | bc)

echo
echo ----------------------------------------
echo "Test duration: $elapsed seconds"
if (( !SKIP_CORRECTNESS )); then
  echo "Correct hashes count: ${pass_count}"
  echo "Wrong hashes count: ${fail_count}"
else
  echo "Procssed count: ${pass_count}"
fi
echo "Throughput: ${throughput} files/sec"
echo
if [[ $pass_count -ge $DUMMY_FILES_N ]]; then
  echo "ERROR: processed count larger than offered load.
       this indicates cache usage, please adjust \$DUMMY_FILES_N 
       to a larger number";
  echo
  exit 1;
fi;

