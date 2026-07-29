#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

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

merge="$(sed -n '/inline void MergeRoots/,/^    }/p' "$HEADER")"
printf '%s\n' "$merge" | rg -q 'stackRoots_\.clear\(\)'
printf '%s\n' "$merge" | rg -q 'chunk->next = rootOverflowFree_'
printf '%s\n' "$merge" | rg -q 'rootOverflowFree_ = chunk'

rg -q 'GreyWorkQueue rootStaging_' "$COLLECTOR_HEADER"
publish="$(sed -n '/void MarkSweepCollector::PublishRootStaging/,/^}/p' "$COLLECTOR_SOURCE")"
printf '%s\n' "$publish" | rg -q 'while \(!rootStaging_\.empty\(\)\)'
printf '%s\n' "$publish" | rg -q 'AddRefToMarkBatch'
printf '%s\n' "$publish" | rg -q 'FlushMarkBatch'

echo 'MSGC bounded root staging contract holds'
