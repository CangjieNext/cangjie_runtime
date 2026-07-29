#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCAN="$ROOT_DIR/src/Common/GCRefFieldScan.inline.h"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
TRACING="$ROOT_DIR/src/Heap/Collector/TracingCollector.cpp"

rg -q 'ForEachRefFieldGCKnownType\(BaseObject\* obj, TypeInfo\* typeInfo' "$SCAN"
known="$(sed -n '/inline void ForEachRefFieldGCKnownType/,/^}/p' "$SCAN")"
if printf '%s\n' "$known" | rg -q 'typeInfo->HasRefField\(\)'; then
    echo 'known-TypeInfo scanner must not repeat the stable ScanClass gate' >&2
    exit 1
fi
if rg -q 'mArray->GetElementSize\(\)' "$SCAN"; then
    echo 'GC array scanner must hoist element size' >&2
    exit 1
fi

rg -q 'bool DiscoverObject\(BaseObject\* obj, TypeInfo\*\* markedTypeInfo = nullptr\)' "$HEADER"
rg -q 'ForEachRefFieldGCKnownType\(.*obj, typeInfo, refVisitor' "$SOURCE"
rg -q 'MSWeakRefLocalBatch weakRefs;' "$SOURCE"
rg -q 'lease.Complete\(batch.IsEmpty\(\)\)' "$SOURCE"
rg -q 'IsMarkBitSetResolved' "$SOURCE"
rg -q 'MarkBitmapWordResolved' "$SOURCE"
rg -Uq '(?s)#ifdef MRT_USE_MARK_SWEEP.*EnumAndTagRawRoot\(root, rootSet\);.*#else' "$TRACING"

if rg -q 'TraceObjectRefFieldsToGCWork|DrainCurrentBuf|PerPGCWork|TryEnqueueGreyObject' \
    "$HEADER" "$SOURCE"; then
    echo 'retired trusted-grey WorkBuf consumer remains' >&2
    exit 1
fi

echo 'MSGC known-TypeInfo Region-owner scan contract holds'
