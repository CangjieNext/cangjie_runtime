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
require_pattern "$LFSTACK_HEADER" 'size_t PopN\(WorkBuf\*\* out, size_t n\)' \
    'LFStack must provide one-lock batch removal'
require_pattern "$LFSTACK_HEADER" 'void PushChain\(WorkBuf\* head\)' \
    'LFStack must provide one-lock chain publication'
require_pattern "$LFSTACK_HEADER" 'size_t got = freeList_\.PopN\(out, n\);' \
    'WorkBufPool::GetN must use the batch free-list operation'
require_pattern "$LFSTACK_HEADER" 'freeList_\.PushChain\(head\);' \
    'WorkBufPool::PutChain must publish the returned chain in one batch'
require_pattern "$ALLOC_HEADER" 'pool\.PutChain\(chain\);' \
    'AllocBufferMS must return its detached log cache as one chain'
reject_pattern "$ALLOC_HEADER" 'while \(logCache_ != nullptr\)' \
    'AllocBufferMS log-cache drain must not retain the old per-node Put loop'

require_pattern "$ALLOC_SOURCE" 'offsetof\(AllocBufferMS, activeLog_\) == 896' \
    'activeLog_ ABI assertion must remain unchanged'
require_pattern "$ALLOC_SOURCE" 'offsetof\(WorkBuf, len\) == 8' \
    'WorkBuf len ABI assertion must remain unchanged'
require_pattern "$ALLOC_SOURCE" 'offsetof\(WorkBuf, objs\) == 16' \
    'WorkBuf objs ABI assertion must remain unchanged'
require_pattern "$ALLOC_SOURCE" 'WorkBuf::kCapacity == 1024' \
    'WorkBuf capacity ABI assertion must remain unchanged'

echo 'MSGC WorkBuf batch and dual-lane contract holds'
