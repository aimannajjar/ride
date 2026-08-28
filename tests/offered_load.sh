#!/usr/bin/env bash

# TODO: account for fork ceilings

MAX=${1:-10000}

for ((i=0; i<MAX; i++)); do
  touch "./tmp/dummy${i}" &
done

echo "WARNING: Workload process simulating second round of same files"
echo "         only an issue if results exceed ${MAX} as it means cache was used"

for ((i=0; i<MAX; i++)); do
  touch "./tmp/dummy${i}" &
done

for ((i=0; i<MAX; i++)); do
  touch "./tmp/dummy${i}" &
done
