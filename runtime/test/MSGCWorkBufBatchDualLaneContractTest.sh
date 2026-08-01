#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ALLOC_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
BATCH_HEADER="$ROOT_DIR/src/Heap/MCollector/MSMarkBatch.h"
SCHED_HEADER="$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.h"
REGION_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSRegionInfo.h"

rg -q 'activeRegions_\[kNumSizeClasses\]\[kMSScanClassCount\]' "$ALLOC_HEADER"
rg -q 'using MSMarkWordBatch = MSMarkWordBatchStorage<16>;' "$BATCH_HEADER"
rg -q 'static constexpr size_t kWays = 2;' "$BATCH_HEADER"
rg -q 'static constexpr size_t kCapacity = kSetCount \* kWays;' "$BATCH_HEADER"
rg -q 'uint32_t regionIdx' "$BATCH_HEADER"
rg -q 'uint8_t wordIdx' "$BATCH_HEADER"
rg -q 'uint64_t mask' "$BATCH_HEADER"
rg -q 'MSMarkReadyStack regionReady_;' "$SCHED_HEADER"
rg -q 'MSMarkReadyStack objectReady_;' "$SCHED_HEADER"
rg -q 'pendingWordSummary' "$REGION_HEADER"
rg -q 'markScheduler_\.TryPublishRegion' "$COLLECTOR_SOURCE"
rg -q 'markScheduler_\.PublishObject' "$COLLECTOR_SOURCE"

if rg -q 'WorkBuf|PerPGCWork|activeLog_|globalFull_|workBufPool_' \
    "$ALLOC_HEADER" "$COLLECTOR_HEADER" "$COLLECTOR_SOURCE"; then
    echo 'ordinary MSGC WorkBuf lane still exists' >&2
    exit 1
fi

echo 'MSGC MarkBatch and REGION/OBJECT dual-ready contract holds'
