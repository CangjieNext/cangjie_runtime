#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
MANAGER="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
SPACE="$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
CENTRAL_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
POLICY="$ROOT_DIR/src/Heap/MCollector/MSMemoryLimitPolicy.h"

require_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! rg -Uq "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern "$MANAGER" 'ResolveMSMarkTrigger\(' \
    'MSGC mark notification must classify soft-limit crossings'
require_pattern "$MANAGER" 'RequestGC\(GC_REASON_OOM, false\)' \
    'MSGC soft-limit crossing must request synchronous OOM GC'
require_pattern "$SPACE" 'ComputeMSScavengeWatermarkReleaseGap\(' \
    'MSGC Scavenge must use the high/low watermark release policy'
require_pattern "$POLICY" 'ComputeMSReleaseGap\(' \
    'MSGC watermarked Scavenge release must remain bounded by reclaimable bytes'
if rg -q 'ResolveMSScavengeTarget' "$POLICY"; then
    echo 'obsolete MSGC Scavenge target clamp remains' >&2
    exit 1
fi
require_pattern "$COLLECTOR" 'RunOOMEmergencyCycle\(' \
    'MSGC must provide an OOM Mark cycle with a single outer STW boundary'
require_pattern "$COLLECTOR" 'MarkTermination\(true\)' \
    'MSGC OOM Mark cycle must reuse MarkTermination while the world is stopped'
require_pattern "$COLLECTOR" 'PreEnumRoots\(true\)' \
    'MSGC OOM Mark cycle must prepare its mark epoch inside the outer STW'
require_pattern "$COLLECTOR" 'PostMarkTermination\(\);' \
    'MSGC OOM Mark cycle must finish PostMarkTermination before leaving OOM'
require_pattern "$COLLECTOR" 'MSMarkedStats markedStats = centralPool->ComputeMarkedStats\(\);' \
    'MSGC PostMark statistics must use the serial immutable-collect-set scan'
require_pattern "$COLLECTOR" 'PrepareMarkEpochUnderSTW\(gcReason == GC_REASON_OOM\)' \
    'MSGC OOM pre-enum reset must request the maximum helper width'
require_pattern "$CENTRAL_HEADER" 'PrepareMarkEpochUnderSTW\(bool forceMaxHelpers' \
    'Central reset preparation must accept an OOM maximum-helper override'
