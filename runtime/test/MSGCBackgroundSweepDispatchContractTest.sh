#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"

if ! rg -q 'bool RefreshSweepDispatch\(\)' "$HEADER" ||
   ! rg -q 'bool CompleteSweepSlice\(' "$HEADER" ||
   ! rg -q 'bool BackgroundProcessor::RefreshSweepDispatch\(\)' "$SOURCE" ||
   ! rg -q 'bool BackgroundProcessor::CompleteSweepSlice\(' "$SOURCE"; then
    echo "BackgroundProcessor must expose one explicit Sweep completion and dispatch refresh boundary" >&2
    exit 1
fi

if rg -q 'bool HasPendingSweepWork|BackgroundProcessor::HasPendingSweepWork' "$HEADER" "$SOURCE"; then
    echo "side-effecting Sweep dispatch refresh must not masquerade as a pure pending query" >&2
    exit 1
fi

if ! rg -q 'shouldParallelSweep_\.store\(runParallel' "$SOURCE" ||
   ! rg -q 'return RefreshSweepDispatch\(\);' "$SOURCE"; then
    echo "Sweep completion must publish the next dispatch mode before returning" >&2
    exit 1
fi

echo "MSGC Background Sweep dispatch contract holds"
