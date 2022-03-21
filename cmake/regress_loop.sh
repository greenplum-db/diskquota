#!/bin/bash
# Usage:
# regress_loop.sh <loop_times> <regress_command>
# Use 0 or negative number for infinite loop

run_times=$1
count=1

while [ "$run_times" -le 0 ] || [ "$count" -le "$run_times" ]; do
    echo "Run regress ${count} times"
    "${@:2}" || exit 1
    count=$(( "$count" + 1 ))
done
