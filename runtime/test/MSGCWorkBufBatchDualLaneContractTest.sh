#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LFSTACK_HEADER="$ROOT_DIR/src/Common/LFStack.h"
ALLOC_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
ALLOC_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"

require_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! rg -q "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

reject_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if rg -q "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern "$LFSTACK_HEADER" 'WorkBuf\* fullTop_ = nullptr;' \
    'LFStack must retain a dedicated full-ready head'
require_pattern "$LFSTACK_HEADER" 'WorkBuf\* partialTop_ = nullptr;' \
    'LFStack must retain a dedicated partial-ready head'
require_pattern "$LFSTACK_HEADER" 'PopResult PopWithCounts\(\)' \
    'LFStack must return one-lock post-pop counts to GC Main'
require_pattern "$LFSTACK_HEADER" 'Counts GetApproxCounts\(\) const' \
    'LFStack must expose a lock-free post-scan scheduling snapshot'
require_pattern "$LFSTACK_HEADER" 'void RecycleCurrentInput\(\)' \
    'PerPGCWork must recycle a drained input before acquiring more work'
require_pattern "$LFSTACK_HEADER" 'workBufPool_->Put\(input\);' \
    'PerPGCWork must return drained input directly to the global pool'
reject_pattern "$LFSTACK_HEADER" 'PopN\(|PushChain\(|GetN\(|PutChain\(' \
    'cache-only WorkBuf batch APIs must be removed'
reject_pattern "$ALLOC_HEADER" 'logCache_|DrainLogCacheToPool' \
    'AllocBufferMS must not retain a write-barrier WorkBuf cache'

require_pattern "$ALLOC_SOURCE" \
    'offsetof\(AllocBufferMS, activeLog_\) == AllocBufferMS::kActiveLogOffset' \
    'activeLog_ ABI assertion must remain unchanged'
require_pattern "$ALLOC_SOURCE" 'offsetof\(WorkBuf, len\) == 8' \
    'WorkBuf len ABI assertion must remain unchanged'
require_pattern "$ALLOC_SOURCE" 'offsetof\(WorkBuf, capacity\) == 12' \
    'WorkBuf capacity ABI assertion must remain unchanged'
require_pattern "$ALLOC_SOURCE" 'sizeof\(WorkBuf\) == 16' \
    'WorkBuf trailing object array ABI assertion must remain unchanged'

echo 'MSGC WorkBuf direct-pool and dual-lane contract holds'
