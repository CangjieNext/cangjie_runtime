#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MARK_BARRIER="$ROOT_DIR/src/Heap/MBarrier/MSMarkBarrier.cpp"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

require_present() {
    local haystack="$1"
    local pattern="$2"
    local message="$3"
    if ! rg -Uq "$pattern" <<<"$haystack"; then
        echo "$message" >&2
        exit 1
    fi
}

require_absent() {
    local haystack="$1"
    local pattern="$2"
    local message="$3"
    if rg -q "$pattern" <<<"$haystack"; then
        echo "$message" >&2
        exit 1
    fi
}

require_order() {
    local haystack="$1"
    local first="$2"
    local second="$3"
    local message="$4"
    local first_line
    local second_line
    first_line="$(printf '%s\n' "$haystack" | rg -n -m1 "$first" | cut -d: -f1)"
    second_line="$(printf '%s\n' "$haystack" | rg -n -m1 "$second" | cut -d: -f1)"
    if [[ -z "$first_line" || -z "$second_line" || "$first_line" -ge "$second_line" ]]; then
        echo "$message" >&2
        exit 1
    fi
}

extract_function() {
    local signature="$1"
    sed -n "/$signature/,/^}/p" "$MARK_BARRIER"
}

source_text="$(<"$MARK_BARRIER")"
collector_text="$(<"$COLLECTOR_HEADER")"
collector_text+=$'\n'
collector_text+="$(<"$COLLECTOR_SOURCE")"

require_present "$source_text" 'class BarrierMarkContext' \
    'Region-scan barriers must share BarrierMarkContext'
require_present "$source_text" 'void AddOldRef\(BaseObject\* ref\)' \
    'BarrierMarkContext must expose AddOldRef'
require_present "$source_text" 'bool IsActive\(\) const' \
    'BarrierMarkContext must expose the mark-phase activity gate'
require_absent "$source_text" 'void Flush\(\)|GetOrCreateBatch\(\)|BatchStorage|batchInitialized_|MSMarkWordBatch& GetBatch' \
    'BarrierMarkContext must be a non-owning, non-flushing handle'
require_absent "$source_text" 'MSWorkUnitCounter work_' \
    'BarrierMarkContext must not calculate barrier WorkUnits'
require_present "$source_text" 'AllocBufferMS\* allocBuffer_' \
    'BarrierMarkContext must retain the owner AllocBufferMS'
require_present "$source_text" 'MSBarrierMarkBuffer\* buffer_' \
    'BarrierMarkContext must retain the fixed-layout owner buffer'
require_present "$source_text" \
    'AddManagedRefToBarrierBuffer\([[:space:]]*ref, \*buffer_\);' \
    'BarrierMarkContext must add old refs to the persistent owner buffer'
require_absent "$source_text" 'EnqueueRefFast|BulkRefMarker' \
    'legacy scalar/bulk marker helpers must be removed after unification'
require_absent "$collector_text" 'PublishManagedRefForBarrier\(' \
    'the obsolete scalar-only collector publication API must be removed'
require_present "$collector_text" 'struct MSNoopWorkUnitCounter' \
    'collector must compile barrier marking with a no-op WorkUnit policy'
require_present "$collector_text" \
    'void AddManagedRefToBarrierBuffer\([[:space:]]*BaseObject\* ref, MSBarrierMarkBuffer& buffer\);' \
    'collector must expose the persistent barrier buffer add API'
require_present "$collector_text" \
    'void FlushBarrierMarkBuffer\(MSBarrierMarkBuffer& buffer\);' \
    'collector must expose the persistent barrier buffer flush API'

barrier_flush="$(sed -n '/void MarkSweepCollector::FlushBarrierMarkBuffer/,/^}/p' "$COLLECTOR_SOURCE")"
require_absent "$barrier_flush" 'cycleCompletedMarkWU_|work\.Total' \
    'barrier flush must not accumulate cycleCompletedMarkWU'

write_ref="$(extract_function 'static inline void WriteRefField')"
require_present "$write_ref" 'BarrierMarkContext marker' \
    'scalar ref writes must use BarrierMarkContext'
require_present "$write_ref" 'marker\.AddOldRef\(oldRef\)' \
    'scalar ref writes must add the overwritten reference'
require_absent "$write_ref" 'marker\.Flush\(\)' \
    'scalar ref writes must retain pending bits across wrapper return'
require_order "$write_ref" 'marker\.AddOldRef\(oldRef\)' 'MSStoreRawReference' \
    'scalar ref writes must capture the old edge before overwriting the slot'

write_struct="$(extract_function 'void MSBarrierOps::WriteStruct')"
require_present "$write_struct" 'EnqueueStructRefsInRange\(marker, obj, dst, copiedLen\)' \
    'struct writes must feed their old references into BarrierMarkContext'
require_absent "$write_struct" 'marker\.Flush\(\)' \
    'struct writes must use the same persistent flush conditions as scalar barriers'
require_order "$write_struct" 'EnqueueStructRefsInRange' 'memcpy_s' \
    'struct writes must capture all old edges before memcpy'

copy_ref_array="$(extract_function 'void MSBarrierOps::CopyRefArray')"
require_present "$copy_ref_array" 'EnqueueRefArrayRange\(marker, dstField, copiedLen\)' \
    'reference array copies must feed their old references into BarrierMarkContext'
require_absent "$copy_ref_array" 'marker\.Flush\(\)' \
    'reference array copies must not flush merely because the wrapper returns'
require_order "$copy_ref_array" 'EnqueueRefArrayRange' 'memmove_s' \
    'reference array copies must capture all old edges before memmove'

atomic_write="$(extract_function 'void MSMarkBarrier::AtomicWriteReference')"
require_present "$atomic_write" 'marker\.AddOldRef\(expected\)' \
    'atomic writes must add the expected old reference'
require_absent "$atomic_write" 'marker\.Flush\(\)' \
    'atomic writes must share persistent barrier flush conditions'
require_order "$atomic_write" 'marker\.AddOldRef\(expected\)' 'MSCompareExchangeRawReference' \
    'atomic writes must capture expected before compare-exchange'

atomic_swap="$(extract_function 'BaseObject\* MSMarkBarrier::AtomicSwapReference')"
require_present "$atomic_swap" 'marker\.AddOldRef\(expected\)' \
    'atomic swaps must add the expected old reference'
require_absent "$atomic_swap" 'marker\.Flush\(\)' \
    'atomic swaps must share persistent barrier flush conditions'
require_order "$atomic_swap" 'marker\.AddOldRef\(expected\)' 'MSCompareExchangeRawReference' \
    'atomic swaps must capture expected before compare-exchange'

atomic_cas="$(extract_function 'bool MSMarkBarrier::CompareAndSwapReference')"
require_present "$atomic_cas" 'marker\.AddOldRef\(oldRef\)' \
    'atomic CAS must add its expected old reference'
require_absent "$atomic_cas" 'marker\.Flush\(\)' \
    'atomic CAS must share persistent barrier flush conditions'
require_order "$atomic_cas" 'marker\.AddOldRef\(oldRef\)' 'MSCompareExchangeRawReference' \
    'atomic CAS must capture expected before compare-exchange'

echo 'MSGC BarrierMarkContext contract holds'
