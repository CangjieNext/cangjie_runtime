#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
REGION_MANAGER_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
REGION_MANAGER_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h"

reject_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if rg -q "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! rg -q "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

reject_pattern "$COLLECTOR" 'kMSMinConcurrentMarkRunwayBytes|minRunway=' \
    "MSGC mark threshold must not retain a fixed concurrent-mark runway"
reject_pattern "$COLLECTOR" 'g_gcRequests\[GC_REASON_HEU\]\.SetMinInterval' \
    "MSGC initialization must not configure a HEU time interval"
require_pattern "$COLLECTOR_HEADER" \
    'request\.reason != GC_REASON_HEU && request\.ShouldBeIgnored\(\)' \
    "MSGC must bypass the generic time policy for HEU requests"
reject_pattern "$REGION_MANAGER_SOURCE" 'GetHeuRequestCooldownNs|lastHeuRequestNs_' \
    "MSGC Region acquisition must not apply a local HEU cooldown"
reject_pattern "$REGION_MANAGER_HEADER" 'lastHeuRequestNs_' \
    "MSGC Region manager must not retain local HEU cooldown state"
reject_pattern "$REGION_MANAGER_SOURCE" 'GetSweptHeapBytesReference|markPressureProbeInProgress_' \
    "MSGC mark notification must trust the first aggregate threshold crossing"
reject_pattern "$REGION_MANAGER_HEADER" 'markPressureProbeInProgress_' \
    "MSGC Region manager must not retain reference-confirmation probe state"

echo "MSGC heap trigger policy contract: PASS"
