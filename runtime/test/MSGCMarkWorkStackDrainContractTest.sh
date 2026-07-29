#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

rg -q 'void PublishRootStaging\(\);' "$HEADER"
rg -Uq '(?s)void MarkSweepCollector::PublishRootStaging\(\).*while \(!rootStaging_\.empty\(\)\).*AddRefToMarkBatch\(obj, false, batch, work\).*FlushMarkBatch\(batch, work\)' "$SOURCE"
if rg -q 'DrainMarkWorkStack|RunConcurrentMarkLoop|TraceObjectRefFieldsToGCWork' \
    "$HEADER" "$SOURCE"; then
    echo 'ordinary object work-stack drain path remains' >&2
    exit 1
fi

echo 'MSGC root staging drains into MarkBatch before scheduler marking'
