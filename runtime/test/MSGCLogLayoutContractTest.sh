#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RUNTIME="$ROOT_DIR/src/CangjieRuntime.cpp"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
ACCOUNTING_H="$ROOT_DIR/src/Heap/MCollector/MSHeapAccounting.h"
ACCOUNTING_CPP="$ROOT_DIR/src/Heap/MCollector/MSHeapAccounting.cpp"
CENTRAL="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
BACKGROUND="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"
FREE_MANAGER="$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.cpp"
REGION_MANAGER="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
REGION_SPACE="$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"

require_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! rg -q -- "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_absent() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if rg -q -- "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern "$RUNTIME" '\[MS-GC\] config:' \
    'MSGC initialization must report its effective configuration'
require_pattern "$REGION_MANAGER" '\[MS-GC\] heap initialized:' \
    'MSGC initialization must report the compact heap layout'
require_absent "$REGION_MANAGER" 'MSRegionManager initialized' \
    'legacy MSRegionManager initialization dump must be removed'
require_absent "$REGION_SPACE" 'MS subsystem initialized' \
    'legacy MS subsystem initialization dump must be removed'
require_pattern "$ACCOUNTING_H" 'struct MSGCRegionSummary' \
    'MSGC Region logging must use one snapshot type'
require_pattern "$ACCOUNTING_CPP" 'CaptureMSGCRegionSummary' \
    'MSGC Region logging must capture the unified snapshot once'
require_pattern "$COLLECTOR" 'Region summary\(%s\)' \
    'GC begin/end Region summaries must share one formatter'
require_absent "$COLLECTOR" 'Mark region state\[cls=' \
    'per-size-class mark snapshots must be removed'
require_absent "$COLLECTOR" 'unswept=%zu emptyCache=%zu hotFree=%zu' \
    'GC entry must not duplicate Region counts'
require_pattern "$COLLECTOR" 'outsideMark=%.2fMB' \
    'GC entry must report the outside-mark correction in MB'
require_pattern "$COLLECTOR" 'g_gcCount \+ 1' \
    'GC entry must report the completed-cycle ordinal instead of an async task index'
require_absent "$COLLECTOR" 'cycle=%llu' \
    'GC entry must not report the raw GC task index as a cycle number'
require_pattern "$COLLECTOR" 'WriteLog\(false, REPORT, ""\);' \
    'GC boundaries must use a raw blank line without consuming their timestamp prefix'
require_absent "$COLLECTOR" 'retained=%zuB swept=%zuB' \
    'GC entry byte fields must be formatted as MB'
require_pattern "$COLLECTOR" 'committed=%zu\(%.2fMB\)' \
    'Region summaries must format committed capacity as MB'
require_pattern "$COLLECTOR" 'sweptRegions=%zu unsweptRegions=%zu' \
    'Region summaries must report current Sweep progress'
require_pattern "$ACCOUNTING_H" 'size_t sweptRegions = 0;' \
    'Region summaries must retain swept Region count in the shared snapshot'
require_pattern "$ACCOUNTING_H" 'size_t unsweptRegions = 0;' \
    'Region summaries must retain unswept Region count in the shared snapshot'
require_absent "$COLLECTOR" 'committed=%zu(%zuB) end=%p' \
    'Region summaries must not print the raw end address'

for phase in PreEnumRoots EnumRoots ConcurrentMark MarkTermination PostMarkTermination; do
    require_pattern "$COLLECTOR" "=== ${phase} begin ===" \
        "${phase} must report phase entry"
    require_pattern "$COLLECTOR" "=== ${phase} end: total=%lluus ===" \
        "${phase} end must contain only total microseconds"
done

require_pattern "$COLLECTOR" '\[MS-GC\]   EnumRoots stats: rawRootRefs=%zu uniqueHeapRoots=%zu' \
    'EnumRoots must name raw and unique root counts before phase end'
require_pattern "$COLLECTOR" '\[MS-GC\]   Mark control begin: helpers=%zu' \
    'Mark control must be indented within ConcurrentMark'
require_pattern "$COLLECTOR" '\[MS-GC\]   ConcurrentMark stats: totalWU=%llu' \
    'ConcurrentMark must report its WU breakdown before phase end'
require_absent "$COLLECTOR" 'Mark control: tasks=' \
    'Mark control changes must not emit per-adjustment task logs'
require_pattern "$COLLECTOR" '\[MS-GC\]   MarkTermination stats: drainWU=%llu' \
    'MarkTermination must report drain WU and helper usage before phase end'
require_pattern "$CENTRAL" '  Sweep force finish: sweptRegions=' \
    'forced sweep must emit one aggregated count and duration'
require_pattern "$CENTRAL" 'Large sweep: releasedLargeObjects=.*releasedSpans=' \
    'large sweep must report released objects and physical spans'
require_pattern "$CENTRAL" 'liveBytes=%.2fMB inUse=%.2fMB' \
    'post-mark statistics must report active capacity'
require_pattern "$CENTRAL" 'fragmentation=%.1f%% retainedNoTLS=%.2fMB' \
    'post-mark statistics must report the live-byte fragmentation ratio'
require_pattern "$CENTRAL" 'liveRatio=%.1f%% total=%lluus' \
    'post-mark statistics must report non-TLS retention and live ratio'
require_pattern "$COLLECTOR" '\[MS-GC\]   GC threshold: live=%.2fMB' \
    'threshold update must use MB'
require_pattern "$CENTRAL" '\[MS-GC\]   Publish sweep: epoch=%u' \
    'Sweep publication must be indented within PostMarkTermination'

require_pattern "$BACKGROUND" 'Sweep round end:' \
    'background sweep must log one round completion'
require_pattern "$BACKGROUND" 'Scavenge round end:' \
    'background Scavenge must log one round completion'
require_absent "$BACKGROUND" 'round begin:|parallel-sweep start|parallel-sweep done|ScavengeSlice' \
    'background logging must only emit complete-round reports'
require_pattern "$REGION_MANAGER" 'Mark notify accepted:' \
    'accepted Mark requests must report their trigger snapshot'
require_pattern "$COLLECTOR" 'Sweep notify accepted: source=post-mark' \
    'accepted Sweep requests must report their trigger snapshot'
require_pattern "$REGION_MANAGER" 'Scavenge notify accepted: source=allocation-pressure' \
    'accepted allocation-pressure Scavenge requests must report their trigger snapshot'
require_pattern "$COLLECTOR" 'Scavenge notify accepted: source=post-mark' \
    'accepted Scavenge requests must report their trigger snapshot'
require_pattern "$FREE_MANAGER" 'segments=%zu->%zu shardSegments=' \
    'heap growth must report segment topology per shard'
require_pattern "$REGION_MANAGER" '\[MS-GC\] segment release:' \
    'physical segment release must report the actual successful release'

echo 'MSGC GC log layout contract holds'
