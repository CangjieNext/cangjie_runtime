#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

require_absent() {
    local path="$1"
    local pattern="$2"
    local message="$3"
    if rg -n --glob '*.{cpp,h,md}' "$pattern" "$path"; then
        echo "$message" >&2
        exit 1
    fi
}

require_absent "$ROOT_DIR/src" 'CJ_MSGC_BARRIER_COUNT' \
    'MSGC barrier diagnostic environment control must be removed'
require_absent "$ROOT_DIR/src" 'MSBarrierRuntimeStat|RecordMSBarrierRuntimeStat|IsMSBarrierRuntimeStatsEnabled|MSGC_BARRIER_STATS' \
    'MSGC barrier diagnostic counters and dumps must be removed'
require_absent "$ROOT_DIR/MSGC_RESULT/Docs" 'CJ_MSGC_BARRIER_COUNT|MSGC_BARRIER_STATS' \
    'MSGC documentation must not describe removed barrier diagnostics'

echo "MSGC barrier diagnostics removal contract: PASS"
