#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CANGJIE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
LLVM_ABI="$CANGJIE_ROOT/cangjie_compiler_region_mark/third_party/llvm-project/llvm/include/llvm/CodeGen/CJMSGCABI.h"
ALLOC_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
ALLOC_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
FAST_STATE="$ROOT_DIR/src/Heap/MSGCFastPath.h"

require() {
    if ! rg -Uq "$2" "$1"; then
        echo "$3" >&2
        exit 1
    fi
}

reject() {
    if rg -q "$2" "$1"; then
        echo "$3" >&2
        exit 1
    fi
}

require "$ALLOC_HEADER" 'class alignas\(64\) AllocBufferMS' \
    'AllocBufferMS must be 64-byte aligned for set cache-line placement'
require "$ALLOC_HEADER" \
    'MSRegionInfo\* activeRegions_\[kNumSizeClasses\]\[kMSScanClassCount\][[:space:]]*= \{\};[[:space:][:print:]]*MSBarrierMarkBuffer barrierMarkBuffer_' \
    'MSBarrierMarkBuffer must immediately follow the 576-byte active Region table'
require "$ALLOC_SOURCE" 'alignof\(AllocBufferMS\) == 64' \
    'Runtime must assert the AllocBufferMS alignment ABI'
require "$ALLOC_SOURCE" 'offsetof\(AllocBufferMS, barrierMarkBuffer_\) == 576' \
    'Runtime must assert the barrier buffer offset'
require "$FAST_STATE" 'offsetof\(MSGCFastPathState, currentMarkEpoch\) == 0' \
    'Runtime must publish currentMarkEpoch at offset zero'
reject "$FAST_STATE" 'barrierPhase|MSGCBarrierPhase|MARK_TLS_BATCH_V3' \
    'Runtime fast state must not duplicate the thread GC phase'

require "$LLVM_ABI" 'MSGCBarrierBufferOffsetInAllocBuffer[[:space:]]*=[[:space:]]*576' \
    'LLVM must mirror the barrier buffer AllocBufferMS offset'
require "$LLVM_ABI" 'MSGCBarrierBufferEntriesOffset[[:space:]]*=[[:space:]]*0' \
    'LLVM must mirror the entries offset'
require "$LLVM_ABI" 'MSGCFastPathCurrentMarkEpochOffset[[:space:]]*=[[:space:]]*0' \
    'LLVM must mirror the current mark epoch offset'
require "$LLVM_ABI" 'MSGCBarrierBufferOccupancyOffset[[:space:]]*=[[:space:]]*512' \
    'LLVM must mirror the occupancy offset'
require "$LLVM_ABI" 'MSGCBarrierBufferPendingBitCountOffset[[:space:]]*=[[:space:]]*516' \
    'LLVM must mirror the pending-bit offset'
require "$LLVM_ABI" 'MSGCBarrierBufferLastEntryOffset[[:space:]]*=[[:space:]]*520' \
    'LLVM must mirror the last-entry offset'
require "$LLVM_ABI" 'MSGCBarrierBufferEntryStride[[:space:]]*=[[:space:]]*16' \
    'LLVM must mirror the entry stride'
require "$LLVM_ABI" 'MSGCBarrierBufferSize[[:space:]]*=[[:space:]]*528' \
    'LLVM must mirror the buffer size'
require "$LLVM_ABI" 'MSGCBarrierBufferCapacity[[:space:]]*=[[:space:]]*32' \
    'LLVM must mirror the buffer capacity'
require "$LLVM_ABI" 'MSGCBarrierBufferSetCount[[:space:]]*=[[:space:]]*8' \
    'LLVM must mirror the set count'
require "$LLVM_ABI" 'MSGCBarrierBufferWays[[:space:]]*=[[:space:]]*4' \
    'LLVM must mirror the ways'
require "$LLVM_ABI" 'MSGCBarrierBufferHashMultiplier[[:space:]]*=[[:space:]]*5' \
    'LLVM must mirror the hash multiplier'
require "$LLVM_ABI" 'MSGCBarrierBufferPendingBitFlushThreshold[[:space:]]*=[[:space:]]*128' \
    'LLVM must mirror the unique pending-bit threshold'
reject "$LLVM_ABI" 'MSGCBarrierBufferEpochOffset|barrier_phase|MSBarrierMarkBufferAbiV3' \
    'LLVM must not retain the deleted buffer epoch or duplicate phase ABI'

echo 'MSGC Runtime and LLVM barrier mark buffer ABI contract holds'
