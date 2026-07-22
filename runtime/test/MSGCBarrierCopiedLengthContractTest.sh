#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MARK_BARRIER="$ROOT_DIR/src/Heap/MBarrier/MSMarkBarrier.cpp"

if ! rg -Uq 'size_t copiedLen = \(dstLen < srcLen \? dstLen : srcLen\);[[:space:]]+EnqueueStructRefsInRange\(obj, dst, copiedLen\);' \
    "$MARK_BARRIER"; then
    echo 'WriteStruct must scan only the copied destination range' >&2
    exit 1
fi

if ! rg -Uq 'size_t copiedLen = \(dstSize < srcSize \? dstSize : srcSize\);[[:space:]]+EnqueueRefArrayRange\(dstField, copiedLen\);' \
    "$MARK_BARRIER"; then
    echo 'CopyRefArray must scan only the copied destination range' >&2
    exit 1
fi

rg -Uq 'memcpy_s\(reinterpret_cast<void\*>\(dst\), dstLen,[[:space:][:print:]]*reinterpret_cast<void\*>\(src\), srcLen\)' \
    "$MARK_BARRIER"
rg -Uq 'memmove_s\(reinterpret_cast<void\*>\(dstField\), dstSize,[[:space:][:print:]]*reinterpret_cast<void\*>\(srcField\), srcSize\)' \
    "$MARK_BARRIER"

echo 'MSGC barrier copied-length contract holds'
