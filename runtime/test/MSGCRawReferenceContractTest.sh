#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IDLE_BARRIER="$ROOT_DIR/src/Heap/MBarrier/MSIdleBarrier.h"
MARK_BARRIER="$ROOT_DIR/src/Heap/MBarrier/MSMarkBarrier.cpp"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
COMPILER_CALLS="$ROOT_DIR/src/CompilerCalls.cpp"
GC_REF_HELPER="$ROOT_DIR/src/Heap/Barrier/GCRefField.inline.h"
COMMON_MSGC_USERS=(
    "$ROOT_DIR/src/CompilerCalls.cpp"
    "$ROOT_DIR/src/Heap/Barrier/Barrier.cpp"
    "$ROOT_DIR/src/Heap/Collector/FinalizerWorkList.cpp"
    "$ROOT_DIR/src/Heap/Collector/TracingCollector.cpp"
    "$ROOT_DIR/src/Mutator/Mutator.cpp"
    "$ROOT_DIR/src/Inspector/CjHeapData.cpp"
    "$ROOT_DIR/src/Inspector/HeapSnapshotJsonSerializer.cpp"
)

require_absent() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if grep -Eq "$pattern" "$file"; then
        echo "$message" >&2
        grep -En "$pattern" "$file" >&2 || true
        exit 1
    fi
}

require_present() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! grep -Eq "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_absent "$IDLE_BARRIER" 'GetTargetObject|SetTargetObject' \
    'MSIdleBarrier must use raw 64-bit MSGC reference helpers'
require_absent "$MARK_BARRIER" 'GetTargetObject|SetTargetObject' \
    'MSMarkBarrier must use raw 64-bit MSGC reference helpers'
require_absent "$COLLECTOR" 'GetTargetObject|SetTargetObject' \
    'MarkSweepCollector must use raw 64-bit MSGC reference helpers'
require_present "$COMPILER_CALLS" 'StoreReferenceForGCMode\(\*field, ref\)' \
    'non-heap-container writes must select reference semantics by GC mode'
require_present "$COMPILER_CALLS" 'LoadReferenceForGCMode\(obj->GetRefField\(offset\)\)' \
    'tuple reflection must load references according to the selected GC mode'

require_present "$GC_REF_HELPER" 'MSLoadRawReference\(field, order\)' \
    'GC-mode reference helper must select raw loads for MSGC'
require_present "$GC_REF_HELPER" 'MSStoreRawReference\(field, ref, order\)' \
    'GC-mode reference helper must select raw stores for MSGC'

for file in "${COMMON_MSGC_USERS[@]}"; do
    require_absent "$file" 'GetTargetObject|SetTargetObject' \
        "common MSGC runtime path must use GC-mode reference helpers: $file"
done

echo 'MSGC raw 64-bit reference contract holds'
