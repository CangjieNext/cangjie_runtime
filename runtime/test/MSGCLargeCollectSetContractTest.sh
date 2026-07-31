#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

rg -q 'std::vector<uint32_t> largeNextSupply_' "$HEADER"
rg -q 'std::vector<uint32_t> largeCollectSet_' "$HEADER"
if rg -q 'largeRegionList_' "$HEADER" "$SOURCE"; then
    echo 'single mutable LARGE membership list remains' >&2
    exit 1
fi

build="$(sed -n '/uint32_t CentralRegionPool::PrepareCollectSetForMarkUnderSTW/,/^}/p' "$SOURCE")"
printf '%s\n' "$build" | rg -q 'largeCollectSet_\.swap\(largeNextSupply_\)'
printf '%s\n' "$build" | rg -q 'region->collectEpoch = epoch'

register="$(sed -n '/void CentralRegionPool::AddLargeLiveRegion/,/^}/p' "$SOURCE")"
printf '%s\n' "$register" | rg -q 'largeNextSupply_\.push_back'

partition="$(sed -n '/void CentralRegionPool::CollectDeadLargeRegions/,/^}/p' "$SOURCE")"
printf '%s\n' "$partition" | rg -q 'for \(uint32_t idx : largeCollectSet_\)'
printf '%s\n' "$partition" | rg -q 'largeNextSupply_\.push_back\(idx\)'
printf '%s\n' "$partition" | rg -q 'largeCollectSet_\.clear\(\)'

stats="$(sed -n '/MSMarkedStats CentralRegionPool::ComputeMarkedStats/,/^}/p' "$SOURCE")"
printf '%s\n' "$stats" | rg -q 'collectSet_\[cls\]\[scan\]\.ForEach'
printf '%s\n' "$stats" | rg -q 'VisitLargeCollectSetLocked'
if printf '%s\n' "$stats" |
   rg -q 'VisitAllocBufferMS|VisitPartialSweptUnsafe|VisitNonPartialUnsafe|VisitLargeRegionsLocked'; then
    echo 'ComputeMarkedStats escapes the frozen collect-set' >&2
    exit 1
fi

postmark="$(sed -n '/void MarkSweepCollector::PostMarkTermination/,/^}/p' "$COLLECTOR")"
stats_line="$(printf '%s\n' "$postmark" | rg -n 'ComputeMarkedStats' | cut -d: -f1)"
large_line="$(printf '%s\n' "$postmark" | rg -n 'CollectLargeGarbage' | cut -d: -f1)"
publish_line="$(printf '%s\n' "$postmark" | rg -n 'PublishCollectSetForSweep' | cut -d: -f1)"
if [[ "$stats_line" -ge "$large_line" || "$large_line" -ge "$publish_line" ]]; then
    echo 'PostMark must consume stats before partitioning the frozen boundary' >&2
    exit 1
fi

echo 'MSGC LARGE two-vector collect-set contract holds'
