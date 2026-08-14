#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
SNAPSHOT_HEADER="$ROOT_DIR/src/Heap/MCollector/MSRootSnapshot.h"

rg -q 'kRootStagingReserve = 256' "$HEADER"
rg -q 'kRootOverflowChunkCapacity = 128' "$HEADER"
rg -q 'BaseObject\* roots\[kRootOverflowChunkCapacity\]' "$HEADER"
rg -q 'RootOverflowChunk\* rootOverflowHead_' "$HEADER"
rg -q 'RootOverflowChunk\* rootOverflowFree_' "$HEADER"

init="$(sed -n '/void AllocBufferMS::Init(/,/^}/p' "$SOURCE")"
printf '%s\n' "$init" | rg -q 'stackRoots_\.reserve\(kRootStagingReserve\)'
printf '%s\n' "$init" | rg -q 'new \(std::nothrow\) RootOverflowChunk'

push="$(sed -n '/void AllocBufferMS::PushRoot/,/^}/p' "$SOURCE")"
printf '%s\n' "$push" | rg -q 'stackRoots_\.size\(\) < stackRoots_\.capacity\(\)'
printf '%s\n' "$push" | rg -q 'stackRoots_\.emplace_back\(root\)'
printf '%s\n' "$push" | rg -q 'AcquireRootOverflowChunk\(\)'
printf '%s\n' "$push" | rg -q 'rootOverflowTail_->roots\[rootOverflowTail_->count\+\+\] = root'

drain="$(sed -n '/void DrainRootsTo/,/^    }/p' "$HEADER")"
printf '%s\n' "$drain" | rg -q 'stackRoots_\.clear\(\)'
printf '%s\n' "$drain" | rg -q 'chunk->next = rootOverflowFree_'
printf '%s\n' "$drain" | rg -q 'rootOverflowFree_ = chunk'

rg -q 'MSRootSnapshot rootSnapshot_' "$COLLECTOR_HEADER"
rg -q 'class MSRootSnapshot' "$SNAPSHOT_HEADER"
publish="$(sed -n '/void MarkSweepCollector::PublishRootStaging/,/^}/p' "$COLLECTOR_SOURCE")"
printf '%s\n' "$publish" | rg -q 'rootSnapshot_\.DeduplicateAndPublish'
printf '%s\n' "$publish" | rg -q 'Heap::IsHeapAddress'
rg -q 'size_t DeduplicateAndPublish' "$SNAPSHOT_HEADER"
rg -q 'dedupTable_\.Insert' "$SNAPSHOT_HEADER"
rg -q 'roots_\.clear' "$SNAPSHOT_HEADER"

range="$(sed -n '/void MarkSweepCollector::PublishRootRange/,/^}/p' "$COLLECTOR_SOURCE")"
printf '%s\n' "$range" | rg -q 'MSMarkWordBatch batch'
printf '%s\n' "$range" | rg -Uq 'AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MANAGED>\([[:space:]]*obj, batch, work, publication\)'
printf '%s\n' "$range" | rg -q 'FlushMarkBatch\(batch, work, publication\)'

echo 'MSGC bounded root staging contract holds'
