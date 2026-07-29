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

rg -Uq '(?s)ResolveMarkOwner\(\).*barrierPhase.*__ATOMIC_ACQUIRE' "$BARRIER"
rg -q 'collector->PublishManagedRefForBarrier\(ref\)' "$BARRIER"
rg -q 'collector_->FlushBarrierMarkBatch\(batch_, work_\)' "$BARRIER"

if rg -q 'TraceObjectRefFieldsToGCWork|EnqueueRefInto|PerPGCWork|activeLog_' \
    "$COLLECTOR" "$BARRIER"; then
    echo 'retired object-WorkBuf discovery path remains' >&2
    exit 1
fi

echo 'MSGC batched discovery and Region-owner scan contract holds'
