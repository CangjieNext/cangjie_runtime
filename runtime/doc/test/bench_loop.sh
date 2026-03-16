#!/bin/bash
cd "$(dirname "$0")"
for i in $(seq 1 10); do
    output=$(cjHeapSize=256mb ./main_bench 2>&1)
    rc=$?
    total=$(echo "$output" | grep "Total wall time" | head -1)
    err=$(echo "$output" | grep -E "Out of memory|StackOverflow" | head -1)
    if [ $rc -eq 0 ]; then
        echo "Run $i: OK  $total"
    else
        echo "Run $i: FAIL(exit=$rc)  $err"
    fi
done
