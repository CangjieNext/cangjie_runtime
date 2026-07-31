#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CANGJIE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
AARCH64_EMITTER="$CANGJIE_ROOT/cangjie_compiler_region_mark/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp"

new_key_block="$(
    sed -n '/emitLabel(LNewKey)/,/emitLabel(LStore)/p' "$AARCH64_EMITTER"
)"

if ! rg -Uq \
    'LDRWui\)\.addReg\(W10\)[[:space:][:print:]]*MSGCBarrierBufferPendingBitCountOffset[[:space:][:print:]]*ADDWri\)\.addReg\(W10\)\.addReg\(W10\)' \
    <<<"$new_key_block"; then
    echo 'AArch64 new-key fast-add must keep pendingBitCount out of the X12 entry-address register' >&2
    exit 1
fi

if rg -Uq \
    'LDRWui\)\.addReg\(W12\)[[:space:][:print:]]*MSGCBarrierBufferPendingBitCountOffset[[:space:][:print:]]*ADDXrs\)\.addReg\(X12\)' \
    <<<"$new_key_block"; then
    echo 'AArch64 new-key fast-add aliases pendingBitCount with the X12 entry address' >&2
    exit 1
fi

echo 'AArch64 MSGC barrier register contract holds'
