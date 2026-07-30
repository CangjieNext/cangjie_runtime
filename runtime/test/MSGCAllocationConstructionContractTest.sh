#!/usr/bin/env bash
set -euo pipefail

runtime_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
compiler_calls_h="$runtime_root/src/CompilerCalls.h"
compiler_calls_cpp="$runtime_root/src/CompilerCalls.cpp"
object_manager_h="$runtime_root/src/ObjectManager.h"
object_manager_inline="$runtime_root/src/ObjectManager.inline.h"
mobject_cpp="$runtime_root/src/ObjectModel/MObject.cpp"
marray_inline="$runtime_root/src/ObjectModel/MArray.inline.h"
central_cpp="$runtime_root/src/Heap/MAllocator/MSCentralRegionPool.cpp"
region_space_cpp="$runtime_root/src/Heap/MAllocator/MSRegionSpace.cpp"

if [[ "$(rg -c '^extern "C" ObjRef MCC_NewPinnedObject' "$compiler_calls_h")" -ne 1 ]]; then
    echo "MCC_NewPinnedObject must have exactly one Runtime declaration" >&2
    exit 1
fi
rg -q '^extern "C" ObjRef MCC_NewWeakRefObject' "$compiler_calls_h"
rg -q 'MCC_NewArrayGenericFast3_MSGC' "$compiler_calls_h"
rg -q 'MCC_NewArrayGenericFast3_MSGC' "$compiler_calls_cpp"

if rg -q 'CjAllocData' "$mobject_cpp" "$marray_inline"; then
    echo "managed construction hot paths must not call the unsupported OHOS allocation profiler" >&2
    exit 1
fi

rg -q 'enum class ArrayElementWidth' "$object_manager_h"
rg -q 'ArrayElementWidth::ONE_BYTE' "$compiler_calls_cpp"
if rg -q 'ArrayElemBits|ELEM_(8|16|32|64)B' \
    "$object_manager_h" "$object_manager_inline" "$compiler_calls_cpp"; then
    echo "array construction must use unambiguous byte-width names" >&2
    exit 1
fi

rg -q 'AllocateGenericArray' "$compiler_calls_cpp"
if [[ "$(rg -c 'switch \(componentTypeInfo->GetType\(\)\)' "$compiler_calls_cpp")" -ne 1 ]]; then
    echo "generic two- and three-argument entry points must share one type dispatch" >&2
    exit 1
fi

if rg -q 'ObjectManager::ObjectManager::|static_cast<MIndex>\(nElems\)' "$compiler_calls_cpp"; then
    echo "redundant construction-path casts or qualifiers remain" >&2
    exit 1
fi

if [[ "$(rg -c '^#include "MSAllocBuffer.h"' "$central_cpp")" -gt 1 ]]; then
    echo "MSCentralRegionPool.cpp contains a duplicate MSAllocBuffer include" >&2
    exit 1
fi

if rg -q 'msBuffer2' "$region_space_cpp"; then
    echo "large allocation must reuse the already resolved AllocBufferMS pointer" >&2
    exit 1
fi
