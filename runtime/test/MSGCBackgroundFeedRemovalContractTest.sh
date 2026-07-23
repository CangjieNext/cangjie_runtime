#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MS_FILES=(
    "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
    "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.h"
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionInfo.h"
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionInfo.cpp"
    "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.h"
    "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"
)

for pattern in \
    'cachedRegion_' 'cachedRegionOrigin_' 'MSAllocCacheFillStats' \
    'BackgroundFillCache' 'RegionAcquireOrigin' 'CACHED_LOCAL' \
    'BackgroundFeedLoop' 'ShouldStartPeriodicFeed' 'HasPendingFeedWork' \
    'pendingFeedStats_' 'GetAllAllocBufferCachedRegionCount' 'HasHungryAllocBuffer'; do
    if rg -q "$pattern" "${MS_FILES[@]}"; then
        echo "stale MSGC Feed symbol remains: $pattern" >&2
        exit 1
    fi
done

if rg -q 'BackgroundFillAllocCaches' \
    "$ROOT_DIR/src/Heap/MAllocator" "$ROOT_DIR/src/Heap/MCollector"; then
    echo 'MSGC must not implement the common CopyGC background-fill hook' >&2
    exit 1
fi

if rg -q '\bmsBackgroundFeed\b|cjMSBackgroundFeed' \
    "$ROOT_DIR/src/Cangjie.h" "$ROOT_DIR/src/CangjieRuntimeApi.cpp" \
    "$ROOT_DIR/src/CjScheduler.cpp" "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"; then
    echo 'live MSGC Feed configuration remains' >&2
    exit 1
fi

rg -q 'uint8_t reservedMSBackgroundFeed;' "$ROOT_DIR/src/Cangjie.h"
rg -q 'offsetof\(AllocBufferMS, activeLog_\) == AllocBufferMS::kActiveLogOffset' \
    "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
rg -Uq 'activeLogAbiReserved_\[kActiveLogOffset - sizeof\(RegionInfo\*\) -[[:space:]]*kNumSizeClasses \* sizeof\(MSRegionInfo\*\)\]' \
    "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"

# The obsolete common background-fill wrapper is gone; CopyGC keeps its active
# FeedHungryBuffers notification and implementation path.
if rg -q 'BackgroundFeedStats|BackgroundFillAllocCaches' \
    "$ROOT_DIR/src/Heap/Allocator/Allocator.h" \
    "$ROOT_DIR/src/Heap/Collector/FinalizerProcessor.h"; then
    echo 'obsolete common background-fill wrapper remains' >&2
    exit 1
fi
rg -q 'virtual void FeedHungryBuffers\(\) = 0' "$ROOT_DIR/src/Heap/Allocator/Allocator.h"
rg -q 'NotifyToFeedAllocBuffers' "$ROOT_DIR/src/Heap/Collector/FinalizerProcessor.h"
rg -q 'NotifyToFeedAllocBuffers' "$ROOT_DIR/src/Heap/Allocator/AllocBuffer.cpp"

echo 'MSGC Background Feed removal contract holds'
