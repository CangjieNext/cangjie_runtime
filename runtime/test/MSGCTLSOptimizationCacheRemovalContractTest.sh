#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ALLOC_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
ALLOC_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
CENTRAL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
WORKBUF_HEADER="$ROOT_DIR/src/Common/LFStack.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

reject_pattern()
{
    local file="$1"
    local pattern="$2"
    local message="$3"
    if rg -q "$pattern" "$file"; then
        echo "FAIL: $message"
        exit 1
    fi
}

require_pattern()
{
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! rg -q "$pattern" "$file"; then
        echo "FAIL: $message"
        exit 1
    fi
}

reject_pattern "$ALLOC_HEADER" 'localEmptyRegion(Cache|Lock)_' \
    "AllocBufferMS must not retain a TLS EmptyRegion cache"
reject_pattern "$ALLOC_HEADER" 'TryPushLocalEmptyRegion|RefillLocalEmptyRegionsFromGlobal|DrainLocalEmptyRegions' \
    "AllocBufferMS must not expose local EmptyRegion cache helpers"
reject_pattern "$ALLOC_SOURCE" 'localEmptyRegion(Cache|Lock)_' \
    "AllocBufferMS implementation must not manipulate a TLS EmptyRegion cache"
reject_pattern "$CENTRAL_SOURCE" 'TryPushLocalEmptyRegion|DrainLocalEmptyRegions|ExtractLocalEmptyRegionsInRange' \
    "CentralRegionPool must not visit per-P EmptyRegion caches"

reject_pattern "$ALLOC_HEADER" 'logCache_|logCacheCount_|DrainLogCacheToPool|kLogCacheRefill' \
    "AllocBufferMS must not retain spare write-barrier WorkBufs"
reject_pattern "$WORKBUF_HEADER" 'emptyCache_|emptyCacheCount_|AcquireEmptyBuffer|RecycleEmptyBuffer|FlushEmptyCache' \
    "PerPGCWork must not retain spare WorkBufs"
reject_pattern "$WORKBUF_HEADER" 'PopN\(|PushChain\(|GetN\(|PutChain\(' \
    "cache-only WorkBuf batch APIs must be removed"

require_pattern "$ALLOC_SOURCE" 'central_->TakeGlobalEmptyRegion\(\)' \
    "EmptyRegion refill must use the central global cache directly"
require_pattern "$COLLECTOR_SOURCE" 'workBufPool_\.Get\(\)' \
    "write-barrier logs must be acquired directly from WorkBufPool"
require_pattern "$WORKBUF_HEADER" 'workBufPool_->Get\(\)' \
    "PerPGCWork output buffers must be acquired directly from WorkBufPool"
require_pattern "$WORKBUF_HEADER" 'workBufPool_->Put\(' \
    "PerPGCWork drained buffers must be returned directly to WorkBufPool"

echo "PASS: MSGC TLS optimization caches are removed"
