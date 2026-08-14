#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WORK_UNIT="$ROOT_DIR/src/Heap/MCollector/MSWorkUnit.h"
SCHEDULER="$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.h"

if ! rg -q 'uint64_t SaturatingMSWUAdd' "$WORK_UNIT"; then
    echo "generic WorkUnit saturation must live with WorkUnit accounting" >&2
    exit 1
fi

if rg -q 'MSAssistPacing.h' "$SCHEDULER" \
    "$ROOT_DIR/src/CangjieRuntimeApi.cpp" "$ROOT_DIR/src/CjScheduler.cpp"; then
    echo "non-assist callers must not depend on MSAssistPacing" >&2
    exit 1
fi

echo "MSGC WorkUnit dependency boundary contract holds"
