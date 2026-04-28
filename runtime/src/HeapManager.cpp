// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "HeapManager.h"

#include "Heap/Heap.h"
#ifdef MRT_USE_MARK_SWEEP
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Mutator/ThreadLocal.h"
#endif

namespace MapleRuntime {
namespace {
Heap* g_cachedHeap = nullptr;
#ifdef MRT_USE_MARK_SWEEP
RegionSpaceMS* g_cachedMSAllocator = nullptr;
#endif

inline Heap& GetManagedHeap()
{
    if (LIKELY(g_cachedHeap != nullptr)) {
        return *g_cachedHeap;
    }
    Heap& heap = Heap::GetHeap();
    g_cachedHeap = &heap;
    return heap;
}
} // namespace

HeapManager::HeapManager() {}

MAddress HeapManager::Allocate(size_t allocSize, AllocType allocType)
{
    return Allocate(allocSize, allocType, nullptr);
}

MAddress HeapManager::Allocate(size_t allocSize, AllocType allocType, ThreadLocalData* local)
{
#ifdef MRT_USE_MARK_SWEEP
    if (LIKELY(g_cachedMSAllocator != nullptr)) {
        if (local == nullptr) {
            local = ThreadLocal::GetThreadLocalData();
        }
        return g_cachedMSAllocator->Allocate(allocSize, allocType, local);
    }
#endif
    return GetManagedHeap().Allocate(allocSize, allocType);
}

void HeapManager::Init(const HeapParam& param)
{
    Heap& heap = Heap::GetHeap();
    g_cachedHeap = &heap;
    heap.Init(param);
#ifdef MRT_USE_MARK_SWEEP
    g_cachedMSAllocator = reinterpret_cast<RegionSpaceMS*>(&heap.GetAllocator());
#endif
}

void HeapManager::Fini()
{
    GetManagedHeap().Fini();
#ifdef MRT_USE_MARK_SWEEP
    g_cachedMSAllocator = nullptr;
#endif
    g_cachedHeap = nullptr;
}
} // namespace MapleRuntime
