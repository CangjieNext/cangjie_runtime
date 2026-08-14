#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TYPE_INFO_MANAGER="$ROOT_DIR/src/TypeInfoManager.cpp"

require() {
    if ! rg -Uq "$2" "$1"; then
        echo "$3" >&2
        exit 1
    fi
}

# Dynamic TypeInfo objects are C++ TypeInfo instances. The bump allocator must
# not return an address that is less aligned than TypeInfo itself.
require "$TYPE_INFO_MANAGER" \
    'constexpr size_t kTypeInfoAllocationAlignment = alignof\(TypeInfo\);' \
    'TypeInfoManager must define the TypeInfo allocation alignment'
require "$TYPE_INFO_MANAGER" \
    'size = MRT_ALIGN\(size, kTypeInfoAllocationAlignment\);' \
    'TypeInfoManager bump allocations must preserve TypeInfo alignment'

echo 'MSGC TypeInfo allocation alignment contract holds'
