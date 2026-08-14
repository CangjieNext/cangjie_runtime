#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
POOL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
REGION_SPACE_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
ASSIST_PACING="$ROOT_DIR/src/Heap/MCollector/MSAssistPacing.h"
SWEEP_POLICY="$ROOT_DIR/src/Heap/MAllocator/MSSweepPressurePolicy.h"
WATERMARK_POLICY="$ROOT_DIR/src/Heap/MCollector/MSScavengeWatermarkPolicy.h"

python3 - "$POOL_SOURCE" "$REGION_SPACE_SOURCE" "$ASSIST_PACING" "$SWEEP_POLICY" "$WATERMARK_POLICY" <<'PY'
import pathlib
import sys

pool = pathlib.Path(sys.argv[1]).read_text()
space = pathlib.Path(sys.argv[2]).read_text()
assist = pathlib.Path(sys.argv[3]).read_text()
sweep = pathlib.Path(sys.argv[4]).read_text()
watermark = pathlib.Path(sys.argv[5]).read_text()

def block(source, begin, end):
    start = source.index(begin)
    finish = source.index(end, start)
    return source[start:finish]

reclaim = block(pool, 'size_t CentralRegionPool::ReclaimEmptyRegion(size_t maxRegions)',
                'bool CentralRegionPool::HasIdleSegmentCachedRegions')
assert 'reserveCount' not in reclaim, 'reclaim must not reserve proportional to the full empty-cache backlog'
assert 'while (' in reclaim and 'kEmptyDrainBatchSize' in reclaim, (
    'reclaim must promote empty Regions in bounded batches'
)
assert reclaim.count('ReclaimReleasedRegionsToHotFreeBatch') == 1, (
    'each bounded batch must use the existing RegionManager batch handoff'
)
assert 'DrainEmptySweptBatch' in reclaim, (
    'bounded reclaim must use the batch-named empty-cache drain helper'
)
assert 'DrainEmptySweptGlobal' not in pool, (
    'the bounded drain helper must not retain its obsolete global name'
)

assert 'ComputeRunwayRegions' not in assist, 'single-use runway rounding must live with Sweep policy arithmetic'
assert 'CeilDivideMSSweepValue' in sweep, 'Sweep policy must provide overflow-safe ceiling division'
assert 'CeilDivideMSSweepValue(' in pool, 'Sweep publication must use centralized safe ceiling division'
assert '(bytesToFlush + MS_REGION_SIZE - 1)' not in space, (
    'central-cache flushing must not use overflow-prone ceiling arithmetic'
)
assert 'one Segment per 10 seconds' in watermark, 'Scavenge high-watermark decay comment must match its 10-second interval'

print('MSGC empty-cache reclaim and policy cleanup contract: PASS')
PY
