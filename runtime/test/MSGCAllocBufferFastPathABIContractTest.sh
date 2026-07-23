#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ALLOC_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
ALLOC_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"

require_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! rg -Uq "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern "$ALLOC_HEADER" \
    'static constexpr size_t kActiveLogOffset = 896;' \
    'AllocBufferMS must define one explicit cross-target activeLog_ ABI offset'
require_pattern "$ALLOC_HEADER" \
    'activeLogAbiReserved_\[kActiveLogOffset - sizeof\(RegionInfo\*\) -[[:space:]]*kNumSizeClasses \* sizeof\(MSRegionInfo\*\)\]' \
    'activeLog_ padding must depend only on the fixed pointer prefix'
require_pattern "$ALLOC_SOURCE" \
    'offsetof\(AllocBufferMS, activeLog_\) == AllocBufferMS::kActiveLogOffset' \
    'the Runtime static assertion must use the shared AllocBufferMS ABI constant'

active_regions_line="$(rg -n 'MSRegionInfo\* activeRegions_' "$ALLOC_HEADER" | cut -d: -f1)"
padding_line="$(rg -n 'activeLogAbiReserved_' "$ALLOC_HEADER" | cut -d: -f1)"
active_log_line="$(rg -n 'WorkBuf\* activeLog_' "$ALLOC_HEADER" | cut -d: -f1)"
mutex_line="$(rg -n 'std::mutex localEmptyRegionLock_' "$ALLOC_HEADER" | cut -d: -f1)"
vector_line="$(rg -n 'std::vector<BaseObject\*> stackRoots_' "$ALLOC_HEADER" | cut -d: -f1)"

for line in "$active_regions_line" "$padding_line" "$active_log_line" "$mutex_line" "$vector_line"; do
    if [[ -z "$line" ]]; then
        echo 'AllocBufferMS fast-path ABI fields are incomplete' >&2
        exit 1
    fi
done

if [[ "$active_regions_line" -ge "$padding_line" ||
      "$padding_line" -ge "$active_log_line" ||
      "$active_log_line" -ge "$mutex_line" ||
      "$active_log_line" -ge "$vector_line" ]]; then
    echo 'activeLog_ must be in a fixed prefix before all STL/platform-dependent members' >&2
    exit 1
fi

echo 'MSGC AllocBuffer fast-path ABI contract holds'
