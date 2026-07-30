#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
BARRIER="$ROOT_DIR/src/Heap/MBarrier/MSMarkBarrier.cpp"

rg -q 'GetRegionInfoDirect\(addr, regionOffset\)' "$COLLECTOR"
rg -q 'batch.Add\(regionIdx, target.wordIdx, target.bitMask' "$COLLECTOR"
rg -q 'batch.GetMarkBitmap\(regionIdx\)' "$COLLECTOR"
rg -q '__atomic_load_n\(markWord, __ATOMIC_RELAXED\)' "$COLLECTOR"
rg -q '__atomic_fetch_or\(markWord, mask, __ATOMIC_RELAXED\)' "$COLLECTOR"
rg -q 'info->PublishPendingMarkWord\(entry.wordIdx\)' "$COLLECTOR"
rg -q 'markScheduler_\.TryPublishRegion\(info\)' "$COLLECTOR"
rg -q 'VisitMarkPendingRegionsUnderSTW' "$COLLECTOR"
rg -q 'ForEachRefFieldGCKnownType' "$COLLECTOR"
rg -q 'scanBitmap\[wordIdx\] = scanWord' "$COLLECTOR"
rg -q 'markScheduler_\.PublishObject\(target.info\)' "$COLLECTOR"

region_task="$(sed -n '/void MarkSweepCollector::RunRegionTask/,/^}/p' "$COLLECTOR")"
weak_ref_line="$(printf '%s\n' "$region_task" |
    rg -n 'obj->IsWeakRef\(\)' | head -1 | cut -d: -f1)"
has_ref_guard_line="$(printf '%s\n' "$region_task" |
    rg -n 'typeInfo->HasRefField\(\)' | head -1 | cut -d: -f1 || true)"
known_type_scan_line="$(printf '%s\n' "$region_task" |
    rg -n 'ForEachRefFieldGCKnownType' | head -1 | cut -d: -f1)"
if [[ -z "$weak_ref_line" || -z "$has_ref_guard_line" ||
      -z "$known_type_scan_line" ||
      "$weak_ref_line" -ge "$has_ref_guard_line" ||
      "$has_ref_guard_line" -ge "$known_type_scan_line" ]]; then
    echo 'Region owner must skip no-ref objects after weak-reference handling' >&2
    exit 1
fi

rg -Uq '(?s)ResolveMarkOwner\(.*GetThreadLocalData\(\)->encodedGCPhase' "$BARRIER"
rg -q 'collector_->AddManagedRefToBarrierBuffer\(ref, \*buffer_\)' "$BARRIER"
rg -q 'collector_->PublishManagedRefForBarrierImmediately\(ref\)' "$BARRIER"
if rg -q 'AddManagedRefToMarkBatch|FlushBarrierMarkBatch|PublishManagedRefForBarrier\(' \
    "$COLLECTOR" "$BARRIER"; then
    echo 'obsolete stack-local barrier batch API remains' >&2
    exit 1
fi

if rg -q 'TraceObjectRefFieldsToGCWork|EnqueueRefInto|PerPGCWork|activeLog_' \
    "$COLLECTOR" "$BARRIER"; then
    echo 'retired object-WorkBuf discovery path remains' >&2
    exit 1
fi

echo 'MSGC batched discovery and Region-owner scan contract holds'
