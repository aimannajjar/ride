#!/usr/bin/env bash
set -euo pipefail

cleanup() {
  sudo kill "$PID" 2>/dev/null || true
}

## Quick functional test for smoke testing
declare -A EXPECTED
EXPECTED["test1.txt"]="c7a0610dcb62188f14592f425924265085ecaf51c85c25967a240994c6c129cd";
EXPECTED["test2.txt"]="94163d7ed3947ba41ef7133a887828f232342387261c904d0e85ccdaaf5aab51";
EXPECTED["test3.txt"]="e21a9f4352397c6fdcf462fa0bbf992c868e5d88d02e46f1ba1abb43d66e30e7";
EXPECTED["test4.bin"]="55d49fd66d2522ab7ea4bdf04472b05eb56e784447f4625c38a52d0072ead2b7";

RIDE_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RIDE="${RIDE_ROOT}/build/ride"
FIXTURES="${RIDE_ROOT}/tests/fixtures"

if [ ! -f "$RIDE" ]; then
  echo "./build/ride was not found, did you build the project?"
  exit 1
fi

sudo -v
rm -f "${RIDE_ROOT}/output.txt"
echo "> Starting RIDE"
trap cleanup EXIT
(sudo "$RIDE" "$FIXTURES" -t 2 -c 2) > "${RIDE_ROOT}/output.txt" &
sleep 0.5;
PID=$(pgrep "ride")

# simulate reads
echo "> Simulate reads"
touch "$FIXTURES"/test1.txt
touch "$FIXTURES"/test2.txt
touch "$FIXTURES"/test3.txt
touch "$FIXTURES"/test4.bin
sleep 1;
sudo kill "$PID" || true
sleep 1;

# validate hashes
echo "> Validate hashes"
expected=4
while read -r line; do
  if ! file=$(echo "$line" | jq -r .file 2>/dev/null); then
    continue;
  fi
  if ! hash=$(echo "$line" | jq -r .hash 2>/dev/null); then
    continue;
  fi

  test="$(basename "$file")"
  echo "> Checking ${test}"

  if [[ "$hash" != "${EXPECTED["$test"]}" ]]; then
    echo "File ${file} hash is wrong; expected=${EXPECTED["$test"]}; got=${hash}";
  else
    echo "PASSED"
    expected=$((expected - 1));
  fi
done < "${RIDE_ROOT}/output.txt"

if [[ "$expected" -gt 0 ]]; then
  echo "Some tests failed or not all files appeared in outout";
  echo
  echo "Full RIDE output:"
  cat "${RIDE_ROOT}/output.txt"
  exit 1
fi

echo --------------------
echo "Full RIDE output:"
cat "${RIDE_ROOT}/output.txt"
echo --------------------

echo "All tests passed"


