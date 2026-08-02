#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

rg -q 'void PublishRootStaging\(\);' "$HEADER"
rg -Uq '(?s)void MarkSweepCollector::PublishRootStaging\(\).*Heap::IsHeapAddress\(obj\).*rootDedupTable_\.Insert.*PublishRootRange.*roots\.clear' "$SOURCE"
rg -Uq '(?s)void MarkSweepCollector::PublishRootRange\(.*MSMarkWordBatch batch.*publication = batch\.Publication\(\).*AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MANAGED>\(.*obj, batch, work, publication\).*FlushMarkBatch\(batch, work, publication\)' "$SOURCE"
if rg -q 'DrainMarkWorkStack|RunConcurrentMarkLoop|TraceObjectRefFieldsToGCWork' \
    "$HEADER" "$SOURCE"; then
    echo 'ordinary object work-stack drain path remains' >&2
    exit 1
fi

echo 'MSGC root staging drains into MarkBatch before scheduler marking'
