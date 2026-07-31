#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MARK_BARRIER="$ROOT_DIR/src/Heap/MBarrier/MSMarkBarrier.cpp"

if ! rg -Uq 'CHECK_DETAIL\(srcLen <= dstLen,[[:space:][:print:]]+\);[[:space:]]+BarrierMarkContext marker;[[:space:]]+EnqueueStructRefsInRange\(marker, obj, dst, srcLen\);' \
    "$MARK_BARRIER"; then
    echo 'WriteStruct must reject invalid lengths before scanning exactly srcLen bytes' >&2
    exit 1
fi

if ! rg -Uq 'CHECK_DETAIL\(srcSize <= dstSize,[[:space:][:print:]]+\);[[:space:]]+BarrierMarkContext marker;[[:space:]]+EnqueueRefArrayRange\(marker, dstField, srcSize\);' \
    "$MARK_BARRIER"; then
    echo 'CopyRefArray must reject invalid lengths before scanning exactly srcSize bytes' >&2
    exit 1
fi

if ! rg -Uq 'EnqueueStaticStructRefs\([[:space:]]*marker, dst, srcLen, gctib\)' \
    "$MARK_BARRIER"; then
    echo 'WriteStaticStruct must limit GCTib enumeration to the overwritten range' >&2
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
