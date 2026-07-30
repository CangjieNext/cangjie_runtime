#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MARK_BARRIER="$ROOT_DIR/src/Heap/MBarrier/MSMarkBarrier.cpp"

if ! rg -Uq 'size_t copiedLen = \(dstLen < srcLen \? dstLen : srcLen\);[[:space:]]+BarrierMarkContext marker;[[:space:]]+EnqueueStructRefsInRange\(marker, obj, dst, copiedLen\);' \
    "$MARK_BARRIER"; then
    echo 'WriteStruct must cache only the copied destination range' >&2
    exit 1
fi

if ! rg -Uq 'size_t copiedLen = \(dstSize < srcSize \? dstSize : srcSize\);[[:space:]]+BarrierMarkContext marker;[[:space:]]+EnqueueRefArrayRange\(marker, dstField, copiedLen\);' \
    "$MARK_BARRIER"; then
    echo 'CopyRefArray must cache only the copied destination range' >&2
    exit 1
fi

if rg -q 'marker\.Flush\(\)' "$MARK_BARRIER"; then
    echo 'MSGC barrier wrappers must not flush the persistent TLS buffer at function return' >&2
    exit 1
fi

rg -Uq 'memcpy_s\(reinterpret_cast<void\*>\(dst\), dstLen,[[:space:][:print:]]*reinterpret_cast<void\*>\(src\), srcLen\)' \
    "$MARK_BARRIER"
rg -Uq 'memmove_s\(reinterpret_cast<void\*>\(dstField\), dstSize,[[:space:][:print:]]*reinterpret_cast<void\*>\(srcField\), srcSize\)' \
    "$MARK_BARRIER"

echo 'MSGC barrier copied-length contract holds'
