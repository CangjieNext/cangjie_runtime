// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "HeapManager.h"

#include "Heap/Heap.h"
#ifdef MRT_USE_MARK_SWEEP
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Heap/MAllocator/MSScanClass.h"
#endif

namespace MapleRuntime {
HeapManager::HeapManager() {}

MAddress HeapManager::Allocate(size_t allocSize, AllocType allocType)
{
    return Heap::GetHeap().Allocate(allocSize, allocType);
}

#ifdef MRT_USE_MARK_SWEEP
MAddress HeapManager::AllocateMS(size_t allocSize, MSScanClass scanClass,
                                 ThreadLocalData* local)
{
    auto& allocator = reinterpret_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    return allocator.Allocate(allocSize, scanClass, local);
}
#endif

void HeapManager::Init(const HeapParam& param) { Heap::GetHeap().Init(param); }

void HeapManager::Fini() { Heap::GetHeap().Fini(); }
} // namespace MapleRuntime
