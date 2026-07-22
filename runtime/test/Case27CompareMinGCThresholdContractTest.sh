#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$ROOT_DIR/MSGC_RESULT/Result/case27_http_complex_json/run_compare.sh"

require_pattern() {
    local pattern="$1"
    local message="$2"
    if ! rg -q "$pattern" "$SCRIPT"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern 'MSGC_MIN_GC_THRESHOLD="\$\{MSGC_MIN_GC_THRESHOLD:-5mb\}"' \
    "case27 compare must default MSGC_MIN_GC_THRESHOLD to 5mb"
require_pattern 'cj_ms_min_gc_threshold=\$\{runtime_min_gc_threshold:-\}' \
    "case27 metadata must record the effective MSGC floor"
require_pattern 'RUNTIME_MIN_GC_THRESHOLD=\$runtime_min_gc_threshold' \
    "case27 isolated child environment must carry the MSGC floor"
require_pattern 'export cjMSMinGCThreshold="\$RUNTIME_MIN_GC_THRESHOLD"' \
    "case27 MSGC child must export cjMSMinGCThreshold"
require_pattern '"\$MSGC_MIN_GC_THRESHOLD"' \
    "case27 MSGC run must pass the configured floor to run_side"

echo "case27 MSGC minimum GC-threshold parameter contract holds"
