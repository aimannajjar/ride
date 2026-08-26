#!/usr/bin/env bash

rm -rf tmp && mkdir ./tmp
sudo -v
(sudo ../build/ride) > ./tmp/output.txt &
PID=$!

start=$EPOCHREALTIME
for i in {1..1000}; do
  touch "./tmp/dummy${i}" &
done
sleep 1; # will deflate througput but that's ok since we're applying consistently
         # need it process last message until we gracefully handle interrupts
kill $PID
wait $PID
finish=$EPOCHREALTIME
elapsed=$(awk -v s="$start" -v e="$finish" 'BEGIN {printf "%.3f", e - s }')
processed=$(cat tmp/output.txt|wc -l)
throughput=$(echo "scale=2; $processed/$elapsed" | bc)
last_dummy=$(cat tmp/output.txt| grep dummy | sort | tail -1)
last_dummy=${last_dummy: -3}

echo "Test duratoin: $elapsed seconds"
echo "Dummy file opens count: ${i}"
echo "Last Dummy processed: ${last_dummy}"
echo "Processed count: ${processed}"
echo "Throughput: ${throughput} files/sec"

