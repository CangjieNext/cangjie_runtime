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

require_pattern "$COLLECTOR_SOURCE" 'size_t baseBytes = cycleStats_\.trueLiveBytes;' \
    "the next MSGC heap goal must use the completed marked-live sample directly"
require_pattern "$COLLECTOR_SOURCE" \
    'cycleStats_\.trueLiveBytes = markedStats\.liveBytes;' \
    "the GC base must be the frozen marked-live bytes"
require_pattern "$COLLECTOR_SOURCE" \
    'GetSmallSweptRegionCount' \
    "central outside Small supply must be snapshotted separately from the GC base"
require_pattern "$COLLECTOR_SOURCE" \
    'GetLargeNextSupplySpanCount' \
    "outside LARGE supply must be snapshotted separately from the GC base"
require_pattern "$COLLECTOR_SOURCE" \
    'gcStats\.SetLastMarkOutsideBytes\(outsideMarkBytes\);' \
    "the completed mark must publish its outside-supply HEU correction"
require_pattern "$COLLECTOR_SOURCE" \
    'bool exceptionalBypass = gcStats\.reason == GC_REASON_OOM;' \
    "UpdateGCStats must recognize OOM as an exceptional threshold cycle"

if printf '%s\n' "$collector_text" | rg -q 'EvaluateMSStableLive\('; then
    echo "UpdateGCStats must not replace the latest trueLive sample with a history median" >&2
    exit 1
fi

if ! printf '%s\n' "$collector_text" |
    rg -q 'bool bypassTransition =[[:space:]]*exceptionalBypass \|\| oldLastTrueLiveBytes == 0 \|\|'; then
    echo "first accepted and OOM cycles must bypass normal threshold smoothing" >&2
    exit 1
fi

if ! printf '%s\n' "$collector_text" |
    rg -q 'EvaluateMSHeapThreshold\([^;]*bypassTransition\)'; then
    echo "UpdateGCStats must pass bootstrap/OOM bypass to threshold evaluation" >&2
    exit 1
fi

if printf '%s\n' "$collector_text" |
    rg -q 'ComputeMSGCThresholdBase|cycleStats_\.thresholdBaseBytes|EvaluateMSScavengeWatermarks\([^;]*outsideCentralSmallBytes|EvaluateMSScavengeWatermarks\([^;]*outsideLargeBytes'; then
    echo "outside allocation supply must not inflate the GC or Scavenge threshold base" >&2
    exit 1
fi

update_gc_stats="$(sed -n '/void MarkSweepCollector::UpdateGCStats()/,/^}/p' "$COLLECTOR_SOURCE")"
if [[ "$(printf '%s\n' "$update_gc_stats" | rg -c 'VLOG\(REPORT')" != "1" ]]; then
    echo "UpdateGCStats must emit one concise REPORT log" >&2
    exit 1
fi

if ! printf '%s\n' "$update_gc_stats" | rg -q '\[MS-GC\] GC threshold:'; then
    echo "UpdateGCStats must report the resulting GC thresholds" >&2
    exit 1
fi

if printf '%s\n' "$update_gc_stats" |
    rg -q 'UpdateGCStats took|heapAdaptive:|heapThreshold stability:|scavengeWatermark:|markEndInUseBytes|inUseBurstBytes'; then
    echo "UpdateGCStats must not retain detailed diagnostic state or timing logs" >&2
    exit 1
fi

echo "MSGC threshold stability collector contract: PASS"
