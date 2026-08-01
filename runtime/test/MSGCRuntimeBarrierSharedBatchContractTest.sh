#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
BATCH_HEADER="$ROOT_DIR/src/Heap/MCollector/MSMarkBatch.h"

rg -q 'class MSBarrierMarkBatchStorageView' "$SOURCE"
rg -q 'enum class MSMarkRefSource' "$COLLECTOR_HEADER"

buffered_add="$(sed -n '/void MarkSweepCollector::AddManagedRefToBarrierBuffer/,/^}/p' "$SOURCE")"
printf '%s\n' "$buffered_add" | rg -q 'MSBarrierMarkBatchStorageView batch'
printf '%s\n' "$buffered_add" | rg -Uq 'AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MANAGED>\([[:space:]]*ref, batch, noWork\)'

buffered_flush="$(sed -n '/void MarkSweepCollector::FlushBarrierMarkBuffer/,/^}/p' "$SOURCE")"
printf '%s\n' "$buffered_flush" | rg -q 'MSBarrierMarkBatchStorageView batch'
printf '%s\n' "$buffered_flush" | rg -q 'FlushMarkBatch\(batch, noWork\)'

publish="$(sed -n '/void MarkSweepCollector::PublishRootRange/,/^}/p' "$SOURCE")"
printf '%s\n' "$publish" | rg -q 'MSMarkWordBatch batch'
printf '%s\n' "$publish" | rg -Uq 'AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MANAGED>\([[:space:]]*obj, batch, work\)'
printf '%s\n' "$publish" | rg -q 'FlushMarkBatch\(batch, work\)'

rg -q 'using MSMarkWordBatch = MSMarkWordBatchStorage<16>;' "$BATCH_HEADER"
if rg -q 'MSRootMarkWordBatch' "$BATCH_HEADER" "$SOURCE"; then
    echo 'EnumRoots must use the same 32-entry local storage as ConcurrentMark' >&2
    exit 1
fi

if rg -q 'AddTrustedRefToMarkBatchImpl|AddDefensiveRefToMarkBatchImpl|AddPreciseChildRefToMarkBatch|FlushMarkBatchImpl' "$SOURCE"; then
    echo 'source-specific Add or split Flush entry remains after unification' >&2
    exit 1
fi

echo 'MSGC Runtime barrier, EnumRoots, and ConcurrentMark share Add/Flush entries'
