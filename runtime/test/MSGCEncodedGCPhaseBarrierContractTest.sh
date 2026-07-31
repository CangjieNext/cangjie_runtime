#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CANGJIE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
LLVM_ROOT="$CANGJIE_ROOT/cangjie_compiler_region_mark/third_party/llvm-project"

FAST_STATE="$ROOT_DIR/src/Heap/MSGCFastPath.h"
HEAP_HEADER="$ROOT_DIR/src/Heap/Heap.h"
HEAP_SOURCE="$ROOT_DIR/src/Heap/Heap.cpp"
MARK_BARRIER="$ROOT_DIR/src/Heap/MBarrier/MSMarkBarrier.cpp"
COMPILER_CALLS="$ROOT_DIR/src/CompilerCalls.cpp"
BUFFER_HEADER="$ROOT_DIR/src/Heap/MBarrier/MSBarrierMarkBuffer.h"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
LLVM_ABI="$LLVM_ROOT/llvm/include/llvm/CodeGen/CJMSGCABI.h"
ASM_PRINTER="$LLVM_ROOT/llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp"
LOWERING="$LLVM_ROOT/llvm/lib/CodeGen/CJBarrierLowering.cpp"
AARCH64_EMITTER="$LLVM_ROOT/llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp"
X86_EMITTER="$LLVM_ROOT/llvm/lib/Target/X86/X86MSGCEmitter.cpp"

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

require "$MARK_BARRIER" \
    'GetThreadLocalData\(\)->encodedGCPhase' \
    'Runtime mark wrapper must judge the current thread encodedGCPhase'
require "$COMPILER_CALLS" \
    'MRT_USE_MARK_SWEEP[[:space:]]+MSBarrierOps::AtomicWriteReference' \
    'MSGC atomic writes must use the TLS phase-aware Runtime operation'
require "$COMPILER_CALLS" \
    'MRT_USE_MARK_SWEEP[[:space:]]+return MSBarrierOps::AtomicSwapReference' \
    'MSGC atomic swaps must use the TLS phase-aware Runtime operation'
require "$COMPILER_CALLS" \
    'MRT_USE_MARK_SWEEP[[:space:]]+return MSBarrierOps::CompareAndSwapReference' \
    'MSGC atomic CAS must use the TLS phase-aware Runtime operation'
require "$COMPILER_CALLS" \
    'MRT_USE_MARK_SWEEP[[:space:]]+\(void\)obj;[[:space:]]+return MSLoadRawReference\(\*field, order\);' \
    'MSGC atomic reads must bypass virtual barrier dispatch'
reject "$FAST_STATE" \
    'barrierPhase|MSGCBarrierPhase|MARK_WRAPPER_ONLY|MARK_TLS_BATCH_V3|PublishMSGCFastPathBarrierPhase' \
    'MSGCFastPathState must not duplicate the GC phase'
reject "$HEAP_HEADER" 'inlineFastPathGCPhase' \
    'Heap must not export a duplicate inline barrier phase'
reject "$HEAP_SOURCE" \
    'inlineFastPathGCPhase|CJ_MS_BARRIER_TLS_BATCH|msBarrierTLSBatchEnabled|PublishMSGCFastPathBarrierPhase' \
    'InstallBarrier must only install the barrier selected by GCPhase'
reject "$BUFFER_HEADER" 'markEpoch' \
    'TLS barrier buffer must not own a mark epoch'
reject "$COLLECTOR" 'buffer\.markEpoch|markBuffer\.markEpoch' \
    'Runtime collector must use currentMarkEpoch instead of buffer.markEpoch'

require "$LLVM_ABI" \
    'MSGCFastPathCurrentMarkEpochOffset[[:space:]]*=[[:space:]]*0' \
    'LLVM must mirror currentMarkEpoch at offset zero'
reject "$LLVM_ABI" \
    'barrier_phase|MSGCBarrierBufferEpochOffset|MSBarrierMarkBufferAbiV3' \
    'LLVM ABI must not retain duplicate phase or buffer epoch contracts'
require "$AARCH64_EMITTER" 'emitGetPackedGCPhaseFromCJTLS' \
    'AArch64 MSGC barrier must use the packed encodedGCPhase'
require "$AARCH64_EMITTER" \
    'ReclaimSATBNode -[[:space:]]+gc_phase::Idle' \
    'AArch64 MSGC barrier must range-check the contiguous idle phases'
require "$AARCH64_EMITTER" \
    'MSMarkTermination -[[:space:]]+gc_phase::MSMarkSetup' \
    'AArch64 MSGC barrier must range-check the contiguous mark phases'
require "$X86_EMITTER" 'getEncodedGCPhaseOffsetInCJTLS' \
    'X86 MSGC barrier must load encodedGCPhase from CJTLS'
require "$X86_EMITTER" \
    'ReclaimSATBNode - gc_phase::Idle' \
    'X86 MSGC barrier must range-check the contiguous idle phases'
require "$X86_EMITTER" \
    'MSMarkTermination - gc_phase::MSMarkSetup' \
    'X86 MSGC barrier must range-check the contiguous mark phases'
require "$AARCH64_EMITTER" \
    'emitLabel\(LSetLookup\);[[:space:][:print:]]*LDRWui\).*MSGCBarrierBufferOccupancyOffset' \
    'AArch64 occupancy must be loaded only after the MRU probe misses'
require "$X86_EMITTER" \
    'emitLabel\(LSetLookup\);[[:space:][:print:]]*EmitLoad32\(X86::R10D, X86::R11,[[:space:]]*layout::MSGCBarrierBufferOccupancyOffset\)' \
    'X86 occupancy must be loaded only after the MRU probe misses'
require "$X86_EMITTER" \
    'MCInstBuilder\(X86::INC32m\)' \
    'X86 fast-add must increment pendingBitCount without reloading it'
reject "$LOWERING" \
    'enable-msgc-write-barrier-lowering|EnableMSGCWriteBarrierLowering' \
    'MSGC write-barrier lowering must not have a separate option'
require "$ASM_PRINTER" \
    'enable-msgc-inline-write-barrier", cl::init\(true\)' \
    'MSGC inline write barrier must remain default-on and configurable'

echo 'MSGC encodedGCPhase write-barrier contract holds'
