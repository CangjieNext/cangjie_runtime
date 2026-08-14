#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ALLOC_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
HEAP_SOURCE="$ROOT_DIR/src/Heap/Heap.cpp"
MARK_BARRIER_SOURCE="$ROOT_DIR/src/Heap/MBarrier/MSMarkBarrier.cpp"

require_present() {
    if ! rg -Uq "$2" "$1"; then
        echo "$3" >&2
        exit 1
    fi
}

require_absent_tree() {
    if rg -q "$2" "$1"; then
        echo "$3" >&2
        exit 1
    fi
}

require_order() {
    local file="$1"
    local first="$2"
    local second="$3"
    local message="$4"
    local first_line
    local second_line
    first_line="$(rg -n -m1 "$first" "$file" | cut -d: -f1 || true)"
    second_line="$(rg -n -m1 "$second" "$file" | cut -d: -f1 || true)"
    if [[ -z "$first_line" || -z "$second_line" || "$first_line" -ge "$second_line" ]]; then
        echo "$message" >&2
        exit 1
    fi
}

require_text_order() {
    local haystack="$1"
    local first="$2"
    local second="$3"
    local message="$4"
    local first_line
    local second_line
    first_line="$(printf '%s\n' "$haystack" | rg -n -m1 "$first" | cut -d: -f1 || true)"
    second_line="$(printf '%s\n' "$haystack" | rg -n -m1 "$second" | cut -d: -f1 || true)"
    if [[ -z "$first_line" || -z "$second_line" || "$first_line" -ge "$second_line" ]]; then
        echo "$message" >&2
        exit 1
    fi
}

# Ordinary safepoint/saferegion paths are intentionally not publication hooks.
require_absent_tree "$ROOT_DIR/src/Mutator" \
    'Flush(Current)?BarrierMarkBuffer|FlushBarrierMarkBuffer|MSBarrierMarkBuffer' \
    'ordinary safepoint/saferegion code must not touch the barrier mark buffer'

fini="$(sed -n '/void AllocBufferMS::Fini()/,/^}/p' "$ALLOC_SOURCE")"
if ! rg -Uq \
    'NotifyAllocBufferExit\(\*this\)[[:space:][:print:]]*ReturnAllRegionsToCentral\(\)[[:space:][:print:]]*RemoveAllocBufferMS\(this\)' \
    <<<"$fini"; then
    echo 'AllocBufferMS::Fini must flush before Region return and registry removal' >&2
    exit 1
fi

notify="$(sed -n '/void MarkSweepCollector::NotifyAllocBufferExit/,/^}/p' "$COLLECTOR_SOURCE")"
if ! rg -q 'FlushBarrierMarkBuffer' <<<"$notify"; then
    echo 'NotifyAllocBufferExit must flush the exiting owner buffer' >&2
    exit 1
fi

require_present "$COLLECTOR_SOURCE" \
    'FlushAllBarrierMarkBuffersUnderSTW' \
    'MarkTermination needs an explicit all-buffer STW drain'
termination="$(sed -n \
    '/void MarkSweepCollector::MarkTermination(bool worldStopped)/,/void MarkSweepCollector::PrepareFinalizersForResurrection()/p' \
    "$COLLECTOR_SOURCE")"
require_text_order "$termination" \
    'activeCollector_\.store\(nullptr' \
    'PublishOrdinaryMarkTasksUnderSTW' \
    'ordinary task publication must run after the collector gate closes'

ordinary_publish="$(sed -n \
    '/void MarkSweepCollector::PublishOrdinaryMarkTasksUnderSTW/,/^}/p' \
    "$COLLECTOR_SOURCE")"
require_text_order "$ordinary_publish" \
    'FlushAllBarrierMarkBuffersUnderSTW' \
    'VisitMarkPendingRegionsUnderSTW' \
    'ordinary task publication must flush TLS buffers before reconciling Regions'
require_present "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp" \
    'PrepareMarkEpochUnderSTW' \
    'CentralPool must reset barrier buffers before publishing the new epoch'

require_present "$MARK_BARRIER_SOURCE" \
    'GetThreadLocalData\(\)->encodedGCPhase' \
    'Runtime wrappers must judge the current mutator encoded GC phase'
require_present "$MARK_BARRIER_SOURCE" \
    'AllocBufferMS::GetOrCreateAllocBufferMS\(\)' \
    'threads without an AllocBufferMS must create and attach an owner buffer'
require_absent_tree "$ROOT_DIR/src/Heap" \
    'PublishManagedRefForBarrierImmediately|OLD_REF_IMMEDIATE_PUBLISH|old_ref_immediate_publish' \
    'Runtime must not retain a separate no-TLS immediate publication path'
require_absent_tree "$HEAP_SOURCE" \
    'CJ_MS_BARRIER_TLS_BATCH|msBarrierTLSBatchEnabled|PublishMSGCFastPathBarrierPhase' \
    'Runtime must not retain a second write-barrier phase switch'

echo 'MSGC barrier mark buffer lifecycle and safepoint non-intervention contract holds'
