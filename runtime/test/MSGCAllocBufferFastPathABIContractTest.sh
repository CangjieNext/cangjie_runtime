#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ALLOC_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
ALLOC_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
REGION_SPACE_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
THREAD_LOCAL_HEADER="$ROOT_DIR/src/Mutator/ThreadLocal.h"

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

require "$ALLOC_HEADER" \
    'MSRegionInfo\* activeRegions_\[kNumSizeClasses\]\[kMSScanClassCount\] = \{\};' \
    'AllocBufferMS compiler prefix must be the 36x2 ScanClass Region table'
require "$ALLOC_SOURCE" \
    'offsetof\(AllocBufferMS, activeRegions_\) == 0' \
    'activeRegions_ must remain at offset zero'
require "$ALLOC_SOURCE" \
    'std::extent<decltype\(activeRegions_\), 1>::value == kMSScanClassCount' \
    'Runtime must assert the inner ScanClass ABI dimension'
require "$ALLOC_SOURCE" \
    'kNumSizeClasses \* kMSScanClassCount \* sizeof\(MSRegionInfo\*\)' \
    'Runtime must assert that the compiler prefix contains only Region pointers'

for pattern in 'activeLog_' 'activeLogAbiReserved_' 'gcWork_' 'WorkBuf'; do
    reject "$ALLOC_HEADER" "$pattern" "retired AllocBuffer ABI member remains: $pattern"
    reject "$ALLOC_SOURCE" "$pattern" "retired AllocBuffer ABI assertion remains: $pattern"
done

ensure="$(sed -n '/void Allocator::EnsureThreadLocalBuffer()/,/^}/p' "$REGION_SPACE_SOURCE")"
printf '%s\n' "$ensure" | rg -q 'AllocBufferMS::GetOrCreateAllocBufferMS\(\)'
if printf '%s\n' "$ensure" | rg -q 'SetAllocBuffer\(|reinterpret_cast<AllocBuffer'; then
    echo 'MSGC EnsureThreadLocalBuffer must not alias AllocBufferMS through CopyGC ThreadLocalData::buffer' >&2
    exit 1
fi

require "$THREAD_LOCAL_HEADER" \
    'static AllocBufferMS\* GetAllocBufferMS\(\) \{ return GetThreadLocalData\(\)->msAllocBuffer; \}' \
    'MSGC must use the dedicated ThreadLocalData::msAllocBuffer field'
reject "$ALLOC_SOURCE" \
    'SetAllocBuffer\(reinterpret_cast<AllocBuffer\*>' \
    'MSGC AllocBuffer creation must not publish a CopyGC buffer alias'

echo 'MSGC AllocBuffer 36x2 fast-path ABI and dedicated TLS field contract holds'
