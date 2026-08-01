#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

rg -q 'class MSBarrierMarkBatchAdapter' "$SOURCE"

buffered_add="$(sed -n '/void MarkSweepCollector::AddManagedRefToBarrierBuffer/,/^}/p' "$SOURCE")"
printf '%s\n' "$buffered_add" | rg -q 'MSBarrierMarkBatchAdapter batch'
printf '%s\n' "$buffered_add" | rg -q 'AddRefToMarkBatchImpl\(ref, true, batch, noWork\)'

buffered_flush="$(sed -n '/void MarkSweepCollector::FlushBarrierMarkBuffer/,/^}/p' "$SOURCE")"
printf '%s\n' "$buffered_flush" | rg -q 'MSBarrierMarkBatchAdapter batch'
printf '%s\n' "$buffered_flush" | rg -q 'FlushMarkBatchImpl\(batch, noWork\)'

publish="$(sed -n '/void MarkSweepCollector::PublishRootRange/,/^}/p' "$SOURCE")"
printf '%s\n' "$publish" | rg -q 'unique_ptr<MSRootMarkWordBatch> batch'
printf '%s\n' "$publish" | rg -q 'AddRefToMarkBatchImpl\(obj, true, \*batch, work\)'
printf '%s\n' "$publish" | rg -q 'FlushMarkBatchImpl\(\*batch, work\)'

echo 'MSGC Runtime barrier and EnumRoots share mark batch core'
