#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

if ! rg -Uq 'WorkEpoch markWorkEpoch_;[[:space:]]*LFStack globalFull_\{&markWorkEpoch_\};' \
    "$COLLECTOR_HEADER"; then
    echo "MSGC Global WorkBuf queue must publish to the mark progress epoch" >&2
    exit 1
fi

if ! rg -q 'MSMarkWorkerControl markWorkerControl_\{&markWorkEpoch_\};' "$COLLECTOR_HEADER"; then
    echo "MSGC worker/assist control must publish to the mark progress epoch" >&2
    exit 1
fi

if ! rg -Uq 'StartMarkWorkers\(size_t maxHelpers, size_t initialHelpers\)[[:space:][:print:]]*SetMaxActiveThreadNum\(static_cast<int32_t>\(maxHelpers\)\)[[:space:][:print:]]*AddWork' \
    "$COLLECTOR_SOURCE"; then
    echo "MARK must restore the shared pool width before submitting long-lived tasks" >&2
    exit 1
fi

if ! rg -q 'MSMarkWorkerControl::Snapshot controlSnapshot = markWorkerControl_\.GetSnapshot\(\);' \
    "$COLLECTOR_SOURCE"; then
    echo "GC main must use one worker/assist control snapshot per convergence check" >&2
    exit 1
fi

if rg -q 'kMSMarkWorkerCreditPollInterval|TakePublishedWorkerCredit|publishedCredit_' \
    "$COLLECTOR_SOURCE" "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h" \
    "$ROOT_DIR/src/Heap/MCollector/MSMarkLocalStats.h"; then
    echo "MSGC Helper must not retain the old background-credit publication path" >&2
    exit 1
fi

if ! rg -q 'size_t backgroundScanned = scannedCount;' "$COLLECTOR_SOURCE"; then
    echo "GC main background credit must only use GC-main scanned work" >&2
    exit 1
fi

if ! rg -q 'markWorkerControl_\.CanContinueWorker\(workerId\)' "$COLLECTOR_SOURCE"; then
    echo "MSGC Helper must continue multiple buffers inside one active claim" >&2
    exit 1
fi

if ! rg -Uq 'PromotePutToGet\([[:space:][:print:]]*workBufPool_\.Capacity\(\) / 8\)[[:space:][:print:]]*AcquireOneBuffer\(\)[[:space:][:print:]]*PromotePutToGet\(1\)' \
    "$COLLECTOR_SOURCE"; then
    echo "MSGC Helper must consume Full/Partial through one Full-first Global claim" >&2
    exit 1
fi

if ! rg -q 'adjustedRatio != publishedAssistRatio' "$COLLECTOR_SOURCE"; then
    echo "GC main must avoid publishing an unchanged assist ratio" >&2
    exit 1
fi

if ! rg -q 'markWorkEpoch_\.WaitForChange' "$COLLECTOR_SOURCE"; then
    echo "GC main must use progress-epoch waiting instead of empty-queue busy looping" >&2
    exit 1
fi

if ! rg -q 'kMSMarkEmptySpinRounds' "$COLLECTOR_SOURCE"; then
    echo "GC main progress waiting must retain a bounded short-spin stage" >&2
    exit 1
fi

echo "MSGC mark-control fast-path contract holds"
