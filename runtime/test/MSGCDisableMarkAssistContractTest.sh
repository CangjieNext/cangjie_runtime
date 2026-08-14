#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
CENTRAL_POOL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
RUNTIME_SOURCE="$ROOT_DIR/src/CangjieRuntime.cpp"
RUNTIME_HEADER="$ROOT_DIR/src/CangjieRuntime.h"
CONFIG_HEADER="$ROOT_DIR/src/Heap/MCollector/MSGCConfig.h"

if ! rg -q 'ReadBoolParameter\("CJ_MS_DISABLE_ASSIST", false\)' "$RUNTIME_SOURCE"; then
    echo "MSGC runtime must initialize the assist diagnostic switch once" >&2
    exit 1
fi

if ! rg -q 'uint8_t assistDisabled' "$CONFIG_HEADER" ||
   ! rg -q 'config\.assistDisabled = input\.assistDisabled != 0 \? 1 : 0' "$CONFIG_HEADER"; then
    echo "MSGC assist diagnostic switch must be normalized by ResolveMSGCConfig" >&2
    exit 1
fi

if ! rg -Uq 'MSGCConfigInput configInput;(?s:.*?)configInput\.assistDisabled =(?s:.*?)ReadBoolParameter\("CJ_MS_DISABLE_ASSIST", false\);(?s:.*?)msAssistDisabled = ResolveMSGCConfig\((?s:.*?)configInput, MSGCConfigSource::ENVIRONMENT\)\.assistDisabled != 0;' "$RUNTIME_SOURCE"; then
    echo "MSGC runtime must route the assist diagnostic switch through the shared resolver" >&2
    exit 1
fi

if ! rg -q 'IsMSAssistDisabled' "$RUNTIME_HEADER"; then
    echo "MSGC runtime must expose the initialized assist diagnostic switch" >&2
    exit 1
fi

if ! rg -q 'CangjieRuntime::IsMSAssistDisabled' "$COLLECTOR_SOURCE" ||
    ! rg -q 'CangjieRuntime::IsMSAssistDisabled' "$CENTRAL_POOL_SOURCE"; then
    echo "MSGC mark and sweep assist must use the shared runtime switch" >&2
    exit 1
fi

if rg -q 'ReadBoolParameter\("CJ_MS_DISABLE_ASSIST", false\)' \
    "$COLLECTOR_SOURCE" "$CENTRAL_POOL_SOURCE"; then
    echo "MSGC mark and sweep paths must not reparse the assist switch" >&2
    exit 1
fi

echo "MSGC shared assist diagnostic switch contract holds"
