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

rg -q 'std::vector<BaseObject\*> rootStaging_' "$COLLECTOR_HEADER"
rg -q 'MSRootDedupTable rootDedupTable_' "$COLLECTOR_HEADER"
publish="$(sed -n '/void MarkSweepCollector::PublishRootStaging/,/^}/p' "$COLLECTOR_SOURCE")"
printf '%s\n' "$publish" | rg -q 'std::vector<BaseObject\*>& roots = rootStaging_'
if printf '%s\n' "$publish" | rg -q 'std::vector<BaseObject\*> roots'; then
    echo 'root publication must reuse collector-owned vector storage' >&2
    exit 1
fi
printf '%s\n' "$publish" | rg -q 'rootDedupTable_\.Insert'
printf '%s\n' "$publish" | rg -q 'Heap::IsHeapAddress\(obj\)'
dedup_line="$(printf '%s\n' "$publish" | rg -n 'rootDedupTable_\.Insert' | cut -d: -f1)"
heap_gate_line="$(printf '%s\n' "$publish" | rg -n 'Heap::IsHeapAddress\(obj\)' | cut -d: -f1)"
resize_line="$(printf '%s\n' "$publish" | rg -n 'roots\.resize\(uniqueRootCount\)' | cut -d: -f1)"
[[ "$dedup_line" -lt "$heap_gate_line" && "$heap_gate_line" -lt "$resize_line" ]]
printf '%s\n' "$publish" | rg -q 'PublishRootRange'
printf '%s\n' "$publish" | rg -q 'roots\.clear'

range="$(sed -n '/void MarkSweepCollector::PublishRootRange/,/^}/p' "$COLLECTOR_SOURCE")"
printf '%s\n' "$range" | rg -q 'MSRootMarkWordBatch batch'
printf '%s\n' "$range" | rg -q 'AddRefToMarkBatchImpl\(obj, true, batch, work\)'
printf '%s\n' "$range" | rg -q 'FlushMarkBatchImpl\(batch, work\)'

immediate_barrier="$(sed -n '/void MarkSweepCollector::PublishManagedRefForBarrierImmediately/,/^}/p' "$COLLECTOR_SOURCE")"
printf '%s\n' "$immediate_barrier" | rg -q 'AddRefToMarkBatchImpl\(ref, true, batch, noWork\)'
printf '%s\n' "$immediate_barrier" | rg -q 'FlushMarkBatchImpl\(batch, noWork\)'

echo 'MSGC bounded root staging contract holds'
