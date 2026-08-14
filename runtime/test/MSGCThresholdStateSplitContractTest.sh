#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

require_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! rg -q "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_collector_text_pattern() {
    local pattern="$1"
    local message="$2"
    if ! printf '%s\n' "$COLLECTOR_TEXT" | rg -q "$pattern"; then
        echo "$message" >&2
        exit 1
    fi
}

COLLECTOR_TEXT="$(tr '\n' ' ' < "$COLLECTOR_SOURCE")"

require_pattern "$COLLECTOR_HEADER" 'UpdateMSHeapThresholdState\(' \
    "MarkSweepCollector must declare the Mark-threshold state helper"
require_pattern "$COLLECTOR_HEADER" 'UpdateMSScavengeWatermarkState\(' \
    "MarkSweepCollector must declare the Scavenge-watermark state helper"
require_pattern "$COLLECTOR_SOURCE" \
    'MarkSweepCollector::UpdateMSHeapThresholdState\(' \
    "MarkSweepCollector must define the Mark-threshold state helper"
require_pattern "$COLLECTOR_SOURCE" \
    'MarkSweepCollector::UpdateMSScavengeWatermarkState\(' \
    "MarkSweepCollector must define the Scavenge-watermark state helper"
require_collector_text_pattern \
    'heapThresholdState[[:space:]]*=[[:space:]]*UpdateMSHeapThresholdState\(' \
    "UpdateGCStats must obtain the Mark threshold from its dedicated helper"
require_collector_text_pattern \
    'scavengeWatermarkState[[:space:]]*=[[:space:]]*UpdateMSScavengeWatermarkState\(' \
    "UpdateGCStats must obtain Scavenge watermarks from their dedicated helper"

echo "MSGC threshold state split contract: PASS"
