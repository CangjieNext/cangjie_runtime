#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

if ! rg -q 'ReadBoolParameter\("CJ_MS_DISABLE_ASSIST", true\)' "$COLLECTOR_SOURCE"; then
    echo "MSGC ConcurrentMark must disable mutator mark assist by default" >&2
    exit 1
fi

if ! rg -q 'ApplyMSAssistDisableOverride' "$COLLECTOR_SOURCE"; then
    echo "MSGC ConcurrentMark must apply the Mark Assist diagnostic override" >&2
    exit 1
fi

if ! rg -q 'disabled=%d effectiveRatio=%llu' "$COLLECTOR_SOURCE"; then
    echo "MSGC Assist pacing log must expose the effective diagnostic state" >&2
    exit 1
fi

echo "MSGC Mark Assist diagnostic override contract holds"
