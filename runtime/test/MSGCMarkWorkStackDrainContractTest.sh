#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

if rg -n 'kSliceMaxObjects|DrainMarkWorkStack\(size_t maxObjects|scannedCount == 0' "$HEADER" "$SOURCE"; then
    echo "markWorkStack drain still uses ineffective object slicing or early-exit state" >&2
    exit 1
fi

if ! rg -q 'void MarkSweepCollector::DrainMarkWorkStack\(\)' "$SOURCE"; then
    echo "markWorkStack must be drained to empty by the parameterless helper" >&2
    exit 1
fi

echo "MSGC markWorkStack drain-to-empty contract holds"
