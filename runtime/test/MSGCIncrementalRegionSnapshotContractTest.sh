#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CENTRAL_H="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
CENTRAL_CPP="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
FREE_H="$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.h"
FREE_CPP="$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.cpp"
SPACE_CPP="$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
MANAGER_CPP="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
BACKGROUND_CPP="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"

require_file_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! grep -Eq "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_file_pattern "$CENTRAL_H" 'centralSmallActiveRegions_' \
    'CentralRegionPool must own the small-active aggregate'
require_file_pattern "$CENTRAL_H" 'centralSmallSweptRegions_' \
    'CentralRegionPool must own the small-swept aggregate'
require_file_pattern "$CENTRAL_H" 'centralSmallUnsweptRegions_' \
    'CentralRegionPool must own the small-unswept aggregate'
require_file_pattern "$CENTRAL_H" 'GetSweptMSRegionCountReference' \
    'CentralRegionPool must retain a swept reference scan'
require_file_pattern "$CENTRAL_H" 'GetActiveMSRegionCountReference' \
    'CentralRegionPool must retain an active reference scan'
require_file_pattern "$CENTRAL_H" 'GetTotalUnsweptCountReference' \
    'CentralRegionPool must retain the 36-class unswept reference scan'
require_file_pattern "$CENTRAL_H" 'forceFinishSweepRunning_' \
    'Sweep completion must track the GC force-finisher'
require_file_pattern "$CENTRAL_H" 'BindAggregateCounters' \
    'SizeClassBucket must publish list transitions into parent aggregates'
require_file_pattern "$CENTRAL_H" 'StealSweptToCollectSet' \
    'PreEnum must steal swept supply into the collect-set'
require_file_pattern "$CENTRAL_H" 'PublishCollectSetToUnswept' \
    'PostMark must publish the collect-set to one unswept head'

require_file_pattern "$FREE_H" 'totalHotFreeRegions_' \
    'FreeManager must own the global hot-free aggregate'
require_file_pattern "$FREE_H" 'GetHotFreeBytesReference' \
    'FreeManager must retain the per-shard hot-free reference scan'
require_file_pattern "$FREE_CPP" 'snapHotFreeRegions_\.exchange' \
    'FreeManager shard publication must compute an aggregate delta'

require_file_pattern "$SPACE_CPP" 'GetSweptHeapBytesReference' \
    'RegionSpace must expose the swept reference snapshot'
require_file_pattern "$SPACE_CPP" 'GetHeapRetainedBytesReference' \
    'RegionSpace must expose the retained reference snapshot'
require_file_pattern "$MANAGER_CPP" 'GetSweptHeapBytesReference\(\)' \
    'Mark notification must confirm a fast threshold crossing'
require_file_pattern "$MANAGER_CPP" 'HasPendingScavengeWork\(\)' \
    'Scavenge notification must confirm a fast threshold crossing against the effective target'
require_file_pattern "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h" 'markPressureProbeInProgress_' \
    'Mark reference confirmation must be coalesced'
require_file_pattern "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h" 'scavengePressureProbeInProgress_' \
    'Scavenge reference confirmation must be coalesced'
require_file_pattern "$MANAGER_CPP" 'expectedProbe, true' \
    'threshold confirmation must have one probe owner'
require_file_pattern "$BACKGROUND_CPP" 'GetScavengeReleaseTargetBytes\(\)' \
    'Background Scavenge must use the complete in-use snapshot and effective target'

require_file_pattern "$CENTRAL_CPP" 'GetSweptMSRegionCountReference' \
    'Central swept reference getter must be implemented'
require_file_pattern "$CENTRAL_CPP" 'GetActiveMSRegionCountReference' \
    'Central active reference getter must be implemented'
require_file_pattern "$CENTRAL_CPP" 'GetTotalUnsweptCountReference' \
    'Central unswept reference getter must be implemented'

python3 - "$CENTRAL_H" "$CENTRAL_CPP" <<'PY'
import pathlib
import sys

header = pathlib.Path(sys.argv[1]).read_text()
source = pathlib.Path(sys.argv[2]).read_text()

method = header.index('void PushPartialSwept(')
aggregate = header.index('EnterCentralList(true);', method)
publish = header.index('partialSwept_.Push(region);', method)
if aggregate > publish:
    raise SystemExit(
        'lock-free partial-swept Push must update the aggregate before publishing the Region')

enter_start = header.index('void EnterCentralList(bool swept)')
enter_end = header.index('void LeaveCentralList(bool swept)', enter_start)
enter = header[enter_start:enter_end]
if 'unsweptRegions_' not in enter:
    raise SystemExit('unswept Push must increment the parent aggregate')

leave_start = enter_end
leave_end = header.index('std::atomic<size_t>* activeRegions_', leave_start)
leave = header[leave_start:leave_end]
if 'unsweptRegions_' not in leave:
    raise SystemExit('normal and stale-head claims must decrement the unswept aggregate')

is_done_start = header.index('bool IsSweepDone() const')
is_done_end = header.index('// ===== Monitoring =====', is_done_start)
if 'forceFinishSweepRunning_.load' not in header[is_done_start:is_done_end]:
    raise SystemExit('IsSweepDone must reject an active GC force-finisher')

publish_set_start = source.index('void CentralRegionPool::PublishCollectSetForSweep()')
publish_set_end = source.index('void CentralRegionPool::BackgroundSweep', publish_set_start)
publish_set = source[publish_set_start:publish_set_end]
if 'centralSmallUnsweptRegions_.store(totalUnswept' not in publish_set:
    raise SystemExit('collect-set publication must publish the exact unswept total')

fast_start = source.index('size_t CentralRegionPool::GetTotalUnsweptCount() const')
reference_start = source.index('size_t CentralRegionPool::GetTotalUnsweptCountReference() const', fast_start)
bytes_start = source.index('size_t CentralRegionPool::GetTotalUnsweptBytes() const', reference_start)
fast_getter = source[fast_start:reference_start]
reference_getter = source[reference_start:bytes_start]
if 'centralSmallUnsweptRegions_.load' not in fast_getter or 'for (' in fast_getter:
    raise SystemExit('normal unswept count getter must remain a single-aggregate fast path')
if 'CountUnswept()' not in reference_getter:
    raise SystemExit('unswept reference getter must scan the unified 36x2 heads')

publish_start = source.index('void CentralRegionPool::PublishSweepDoneAfterQuiescence()')
publish_end = source.index('void CentralRegionPool::MaybeNotifySweep', publish_start)
publish_done = source[publish_start:publish_end]
required_publish_order = [
    'forceFinishSweepRunning_.load',
    'GetTotalUnsweptCount()',
    'GetTotalUnsweptCountReference()',
    'sweepDone_.store(true',
]
cursor = 0
for token in required_publish_order:
    found = publish_done.find(token, cursor)
    if found < 0:
        raise SystemExit(f'Sweep completion missing ordered correctness step: {token}')
    cursor = found + len(token)

stop_start = source.index('void CentralRegionPool::StopAndWaitForAllSweep()')
stop_end = source.index('void CentralRegionPool::ClearAllMarkBitmaps()', stop_start)
stop = source[stop_start:stop_end]
first_drain = stop.index('ForceFinishAllUnswept();')
second_drain = stop.index('ForceFinishAllUnswept();', first_drain + 1)
guard_on = stop.index('forceFinishSweepRunning_.store(true')
guard_off = stop.index('forceFinishSweepRunning_.store(false')
publish = stop.index('PublishSweepDoneAfterQuiescence()')
if not (guard_on < first_drain < second_drain < guard_off < publish):
    raise SystemExit('force-finish guard must cover both drains and clear before final publication')
PY

echo 'MSGC incremental Region snapshot contract holds'
