#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ALLOC_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
ALLOC_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
CENTRAL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

for pattern in 'localEmptyRegion(Cache|Lock)_' 'TryPushLocalEmptyRegion' \
               'RefillLocalEmptyRegionsFromGlobal' 'DrainLocalEmptyRegions' \
               'logCache_' 'activeLog_' 'gcWork_' 'PerPGCWork' \
               'globalFull_' 'workBufPool_'; do
    if rg -q "$pattern" "$ALLOC_HEADER" "$ALLOC_SOURCE" \
        "$CENTRAL_SOURCE" "$COLLECTOR_HEADER" "$COLLECTOR_SOURCE"; then
        echo "retired TLS/WorkBuf cache remains: $pattern" >&2
        exit 1
    fi
done

rg -q 'central_->TakeGlobalEmptyRegion\(\)' "$ALLOC_SOURCE"
rg -q 'MSMarkScheduler markScheduler_;' "$COLLECTOR_HEADER"
rg -q 'MSMarkWordBatch batch;' "$COLLECTOR_SOURCE"

echo 'MSGC TLS caches and ordinary WorkBuf state are removed'
