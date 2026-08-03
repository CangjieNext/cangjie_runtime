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

collector_text="$(tr '\n' ' ' < "$COLLECTOR_SOURCE")"

require_pattern "$COLLECTOR_SOURCE" 'size_t baseBytes = liveBytes;' \
    "the next MSGC heap goal must use the latest completed trueLive sample"
require_pattern "$COLLECTOR_SOURCE" \
    'bool exceptionalBypass = gcStats\.reason == GC_REASON_OOM;' \
    "UpdateGCStats must recognize OOM as an exceptional threshold cycle"

if printf '%s\n' "$collector_text" | rg -q 'EvaluateMSStableLive\('; then
    echo "UpdateGCStats must not replace the latest trueLive sample with a history median" >&2
    exit 1
fi

if ! printf '%s\n' "$collector_text" |
    rg -q 'bool bypassTransition =[[:space:]]*exceptionalBypass \|\| oldLastTrueLiveBytes == 0;'; then
    echo "first accepted and OOM cycles must bypass normal threshold smoothing" >&2
    exit 1
fi

if ! printf '%s\n' "$collector_text" |
    rg -q 'EvaluateMSHeapThreshold\([^;]*bypassTransition\)'; then
    echo "UpdateGCStats must pass bootstrap/OOM bypass to threshold evaluation" >&2
    exit 1
fi

for field in \
    'markRunway=%zuB' \
    'markRunwayEstimate=%zuB' \
    'minThreshold=%zuB' \
    'deadbandLower=%zuB' \
    'deadbandUpper=%zuB' \
    'deadbandHeld=%d' \
    'fallLimit=%zuB' \
    'transitionLimited=%d' \
    'transitionBypassed=%d' \
    'exceptionalBypass=%d'; do
    require_pattern "$COLLECTOR_SOURCE" "$field" \
        "heapThreshold REPORT must expose $field"
done

echo "MSGC threshold stability collector contract: PASS"
