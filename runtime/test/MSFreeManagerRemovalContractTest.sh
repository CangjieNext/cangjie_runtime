#!/usr/bin/env bash
set -euo pipefail

runtime_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
paths=(
    "$runtime_root/src/Heap/MAllocator/MSFreeManager.h"
    "$runtime_root/src/Heap/MAllocator/MSFreeManager.cpp"
    "$runtime_root/src/Heap/MAllocator/MSRegionManager.h"
    "$runtime_root/src/Heap/MAllocator/MSRegionManager.cpp"
    "$runtime_root/test/MSFreeManagerShardStatsTest.cpp"
    "$runtime_root/test/MSFreeManagerFastPathTest.cpp"
    "$runtime_root/test/MSFreeManagerOwnerTest.cpp"
)

removed_pattern='GrowForRegions|ScavengeFreeRegions\(bool releaseAll\)|TryTakeOneRegionFromOwnedViewsLocked|FindNextFreeRegionLocked|FindFreeRunLocked|ownedSegments_|reusedScavengedRegionCount_|GetReusedScavengedRegionCount'

if rg -n "$removed_pattern" "${paths[@]}"; then
    echo "obsolete FreeManager APIs, duplicate topology state, or test-only reuse counters remain" >&2
    exit 1
fi

if rg -n 'MarkScavengingLocked\(shard, spans\[' \
    "$runtime_root/src/Heap/MAllocator/MSFreeManager.cpp"; then
    echo "whole-segment Scavenge must reuse the canonical word-mask claim path" >&2
    exit 1
fi

echo "FreeManager removal contract: PASS"
