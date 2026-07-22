#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GC_PARAM="$ROOT_DIR/src/Cangjie.h"
ENV_HEADER="$ROOT_DIR/src/Base/EnvParam.h"
ENV_SOURCE="$ROOT_DIR/src/Base/EnvParam.cpp"
SCHEDULER="$ROOT_DIR/src/CjScheduler.cpp"
RUNTIME_API="$ROOT_DIR/src/CangjieRuntimeApi.cpp"
GC_STATS="$ROOT_DIR/src/Heap/Collector/GcStats.cpp"
MS_COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

require_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! rg -q "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

reject_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if rg -q "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern "$GC_PARAM" 'size_t msMinGCThreshold;' \
    "GCParam must store the resolved MSGC threshold floor"
require_pattern "$ENV_HEADER" 'bool allowZero = false' \
    "ReadSizeParameter must expose opt-in zero support"
require_pattern "$ENV_SOURCE" 'allowZero && value == "0"' \
    "ReadSizeParameter must accept literal zero only when requested"
require_pattern "$SCHEDULER" 'cjMSMinGCThreshold' \
    "scheduler initialization must resolve cjMSMinGCThreshold"
require_pattern "$RUNTIME_API" 'cjMSMinGCThreshold' \
    "embedded runtime initialization must resolve cjMSMinGCThreshold"
require_pattern "$GC_STATS" 'msMinGCThreshold' \
    "initial GCStats threshold must use the configured MSGC floor"
require_pattern "$MS_COLLECTOR" 'msMinGCThreshold' \
    "MSGC per-cycle threshold must use the configured floor"

reject_pattern "$MS_COLLECTOR" 'static_cast<size_t>\(20\) \* 1024 \* 1024' \
    "MSGC must not retain the hard-coded 20 MB per-cycle floor"

echo "MSGC minimum GC-threshold configuration contract holds"
