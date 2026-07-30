#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CANGJIE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
LLVM_ROOT="$CANGJIE_ROOT/cangjie_compiler_region_mark/third_party/llvm-project"
AARCH64="$LLVM_ROOT/llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp"
X86="$LLVM_ROOT/llvm/lib/Target/X86/X86MSGCEmitter.cpp"
AARCH64_HEADER="$LLVM_ROOT/llvm/lib/Target/AArch64/AArch64AsmPrinter.h"
X86_HEADER="$LLVM_ROOT/llvm/lib/Target/X86/X86AsmPrinter.h"

require() {
    if ! rg -Uq "$2" "$1"; then
        echo "$3" >&2
        exit 1
    fi
}

for emitter in "$AARCH64" "$X86"; do
    require "$emitter" 'MSGCBarrierBufferOffsetInAllocBuffer' \
        "$emitter must address the fixed AllocBufferMS barrier buffer"
    require "$emitter" 'MSGCBarrierBufferPendingBitCountOffset' \
        "$emitter must enforce the unique pending-bit threshold"
    require "$emitter" 'MSGCBarrierBufferOccupancyOffset' \
        "$emitter must publish and probe occupancy"
    require "$emitter" 'MSGCBarrierBufferLastEntryOffset' \
        "$emitter must implement the last-entry fast lookup"
    require "$emitter" 'lCJMSGCMarkSizeClassTable' \
        "$emitter must resolve slots through the compact mark descriptor table"
    require "$emitter" 'MSGCMarkSizeClassDescs' \
        "$emitter must emit the Runtime-mirrored 36-entry mark descriptor table"
done

require "$AARCH64" 'emitGetPackedGCPhaseFromCJTLS' \
    'AArch64 must use the encoded phase cached in the packed CJTLS register'
require "$X86" 'getEncodedGCPhaseOffsetInCJTLS' \
    'X86 must load encodedGCPhase directly from CJTLS'
require "$AARCH64_HEADER" 'MSGCMarkTablesNeeded' \
    'AArch64 AsmPrinter must track mark descriptor table demand independently'
require "$X86_HEADER" 'MSGCMarkTablesNeeded' \
    'X86 AsmPrinter must track mark descriptor table demand independently'

echo 'MSGC AArch64/X86 direct barrier buffer emission contract holds'
