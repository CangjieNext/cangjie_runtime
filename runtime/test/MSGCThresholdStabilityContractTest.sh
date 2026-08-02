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

require_pattern "$COLLECTOR_HEADER" 'MSHeapThresholdState heapThresholdState_;' \
    "MarkSweepCollector must own stable-live threshold history"
require_pattern "$COLLECTOR_SOURCE" \
    'bool exceptionalBypass = gcStats\.reason == GC_REASON_OOM;' \
    "UpdateGCStats must recognize OOM as an exceptional threshold cycle"

if ! printf '%s\n' "$collector_text" |
    rg -q 'EvaluateMSStableLive\([[:space:]]*heapThresholdState_,[^;]*exceptionalBypass\)'; then
    echo "UpdateGCStats must pass collector history and exceptional bypass to stable-live evaluation" >&2
    exit 1
fi

if ! printf '%s\n' "$collector_text" |
    rg -q 'bool bypassTransition =[[:space:]]*exceptionalBypass \|\| !stableDecision\.hadAcceptedHistory;'; then
    echo "first accepted and OOM cycles must bypass normal threshold smoothing" >&2
    exit 1
fi

if ! printf '%s\n' "$collector_text" |
    rg -q 'EvaluateMSHeapThreshold\([^;]*bypassTransition\)'; then
    echo "UpdateGCStats must pass bootstrap/OOM bypass to threshold evaluation" >&2
    exit 1
fi

for field in \
    'historySize=%zu' \
    'medianStableLive=%zuB' \
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
