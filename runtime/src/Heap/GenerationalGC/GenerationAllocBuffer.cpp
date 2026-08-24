// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "GenerationAllocBuffer.h"
#include "Allocator/MixedRegionSpace/MixedRegionSpace.h"
#include "Collector/Collector.h"
#include "Collector/FinalizerProcessor.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"

namespace MapleRuntime {
GenerationAllocBuffer* GenerationAllocBuffer::GetGenerationAllocBuffer()
{
    return static_cast<GenerationAllocBuffer*>(AllocBuffer::GetAllocBuffer());
}

GenerationAllocBuffer::~GenerationAllocBuffer()
{
    MixedRegionSpace& space = MixedRegionSpace::CastDown(Heap::GetHeap().GetAllocator());
    FlushSlotRegionCaches(MixedRegionSpace::CastDown(space.GetRegionManager()));
}

void GenerationAllocBuffer::FlushSlotRegionCaches(MixedRegionManager& manager)
{
    RegionInfo* region = nullptr;
    while ((region = youngSlotRegionCache.TakeHeadRegionNoLock()) != nullptr) {
        region->ClearFreeSlotList();
        manager.RecycleYoungSlotRegion(region);
    }
    while ((region = oldSlotRegionCache.TakeHeadRegionNoLock()) != nullptr) {
        region->ClearFreeSlotList();
        manager.RecycleOldSlotRegion(region);
    }
}

namespace {
MAddress AllocateFromSlotRegionCache(RegionList& cache, size_t totalSize)
{
    MAddress result = 0;
    cache.VisitAllRegionsNoLock([&](RegionInfo* region) {
        if (result != 0) {
            return;
        }
        RegionFreeSlotList* freeSlotList = region->GetFreeSlotList();
        if (freeSlotList == nullptr || freeSlotList->GetMaxAvailableSize() < totalSize) {
            return;
        }
        uintptr_t addr = freeSlotList->AllocateSlot(totalSize, region->GetRegionStart());
        if (addr != 0) {
            CHECK_DETAIL(memset_s(reinterpret_cast<void*>(addr), totalSize, 0, totalSize) == EOK,
                         "zero free slot failed");
            result = static_cast<MAddress>(addr);
        }
    });
    return result;
}

RegionInfo* RemoveSmallestSlotRegion(RegionList& cache)
{
    RegionInfo* smallest = nullptr;
    size_t smallestSize = std::numeric_limits<size_t>::max();
    cache.VisitAllRegionsNoLock([&](RegionInfo* region) {
        RegionFreeSlotList* freeSlotList = region->GetFreeSlotList();
        size_t maxSlotSize = freeSlotList == nullptr ? 0 : freeSlotList->GetMaxAvailableSize();
        if (smallest == nullptr || maxSlotSize < smallestSize) {
            smallest = region;
            smallestSize = maxSlotSize;
        }
    });
    if (smallest != nullptr) {
        cache.RemoveRegionNoLock(smallest);
    }
    return smallest;
}
}
MAddress GenerationAllocBuffer::AllocateImpl(size_t totalSize)
{
    MixedRegionSpace& theAllocator = MixedRegionSpace::CastDown(Heap::GetHeap().GetAllocator());
    MixedRegionManager& manager = MixedRegionSpace::CastDown(theAllocator.GetRegionManager());

    RegionInfo* tr = tlRegion;
    if (LIKELY(tr != RegionInfo::NullRegion())) {
        MAddress addr = tr->Alloc(totalSize);
        if (addr != 0) {
            return addr;
        }

        CHECK(tr->IsThreadLocalRegion());
        // 10: Regard 1/10 capacity as the threshold of the criteria that enlist the region.
        if (tr->GetAvailableSize() < (tr->GetRegionSize() / 10)) {
            manager.RemoveThreadLocalRegion(tr);
            manager.EnlistFullThreadLocalRegion(tr);
            tlRegion = RegionInfo::NullRegion();
        }
    }

    RegionInfo* r = preparedRegion.load(std::memory_order_acquire);
    if (r != nullptr) {
        preparedRegion.store(nullptr, std::memory_order_release);
        tlRegion = r;
        return r->Alloc(totalSize);
    }

    // Take a new region when young space is empty, and Take slot when young space is heavy.
    if (manager.IsYoungSpaceHeavy()) {
        MAddress slot = AllocateYoungOnSlot(manager, totalSize);
        if (slot != 0) { return slot; }
    }
    return AllocateYoungOnNewRegion(manager, totalSize);
}

MAddress GenerationAllocBuffer::AllocateYoungOnNewRegion(MixedRegionManager &manager, size_t totalSize)
{
    CJThreadPreemptOffCntAdd();
    RegionInfo* r = manager.AllocateThreadLocalRegion();
    CJThreadPreemptOffCntSub();
    if (UNLIKELY(r == nullptr))
    {
        return 0;
    }
    if (tlRegion == RegionInfo::NullRegion())
    {
        tlRegion = r;
        return r->Alloc(totalSize);
    }
    MAddress addr = tlRegion->Alloc(totalSize);
    if (addr != 0)
    {
        if (!SetPreparedRegion(r))
        {
            manager.RemoveThreadLocalRegion(r);
            manager.ReclaimRegion(r);
        }
        return addr;
    }
    manager.RemoveThreadLocalRegion(tlRegion);
    manager.EnlistFullThreadLocalRegion(tlRegion);
    tlRegion = r;
    return r->Alloc(totalSize);
}

MAddress GenerationAllocBuffer::AllocateYoungOnSlot(MixedRegionManager &manager, size_t totalSize)
{
    if (MAddress result = AllocateFromSlotRegionCache(youngSlotRegionCache, totalSize)) {
        return result;
    }
    if (youngSlotRegionCache.GetRegionCount() > 0) {
        RegionInfo* region = RemoveSmallestSlotRegion(youngSlotRegionCache);
        if (region != nullptr) {
            region->ClearFreeSlotList();
            manager.RecycleYoungSlotRegion(region);
        }
    }
    RegionInfo* region = manager.GetSweptRegionList().TakeHeadRegion();
    if (region != nullptr) {
        CHECK(youngSlotRegionCache.GetRegionCount() < SLOT_REGION_CACHE_LIMIT);
        youngSlotRegionCache.PrependRegionNoLock(region, RegionInfo::RegionType::SWEEP_REGION);
        if (MAddress result = AllocateFromSlotRegionCache(youngSlotRegionCache, totalSize)) {
            return result;
        }
    }
    MAddress result = 0;
    manager.GetSweptRegionList().VisitAllRegions([&](RegionInfo* region) {
        if (result != 0) {
            return;
        }
        RegionFreeSlotList* freeSlotList = region->GetFreeSlotList();
        if (freeSlotList == nullptr || freeSlotList->GetMaxAvailableSize() < totalSize) {
            return;
        }
        uintptr_t addr = freeSlotList->AllocateSlot(totalSize, region->GetRegionStart());
        if (addr != 0) {
            CHECK_DETAIL(memset_s(reinterpret_cast<void*>(addr), totalSize, 0, totalSize) == EOK,
                         "zero free slot failed");
            result = static_cast<MAddress>(addr);
        }
    });
    return result;
}

MAddress GenerationAllocBuffer::AllocateYoung(size_t totalSize)
{
    return AllocateImpl(totalSize);
}

MAddress GenerationAllocBuffer::AllocateOld(size_t totalSize)
{
    MixedRegionSpace& theAllocator = MixedRegionSpace::CastDown(Heap::GetHeap().GetAllocator());
    MixedRegionManager& manager = MixedRegionSpace::CastDown(theAllocator.GetRegionManager());

    if (LIKELY(oldAllocRegion != RegionInfo::NullRegion())) {
        MAddress addr = oldAllocRegion->Alloc(totalSize);
        if (addr != 0) {
            return addr;
        }

        {
            manager.RemoveThreadLocalRegion(oldAllocRegion);
            manager.EnlistOldRegionList(oldAllocRegion);
            oldAllocRegion = RegionInfo::NullRegion();
        }
    }
    MAddress slot = AllocateOldOnSlot(manager, totalSize);
    if (slot != 0) { return slot; }
    return AllocateOldOnNewRegion(manager, totalSize);
}

MAddress GenerationAllocBuffer::AllocateOldOnSlot(MixedRegionManager &manager, size_t totalSize)
{
    if (MAddress result = AllocateFromSlotRegionCache(oldSlotRegionCache, totalSize)) {
        return result;
    }
    if (oldSlotRegionCache.GetRegionCount() > 0) {
        RegionInfo* region = RemoveSmallestSlotRegion(oldSlotRegionCache);
        if (region != nullptr) {
            region->ClearFreeSlotList();
            manager.RecycleOldSlotRegion(region);
        }
    }
    RegionInfo* region = manager.GetOldSweptRegionList().TakeHeadRegion();
    if (region != nullptr) {
        CHECK(oldSlotRegionCache.GetRegionCount() < SLOT_REGION_CACHE_LIMIT);
        oldSlotRegionCache.PrependRegionNoLock(region, RegionInfo::RegionType::SWEEP_REGION);
        if (MAddress result = AllocateFromSlotRegionCache(oldSlotRegionCache, totalSize)) {
            return result;
        }
    }
    MAddress result = 0;
    manager.GetOldSweptRegionList().VisitAllRegions([&totalSize, &result](RegionInfo* region) {
        if (result != 0) {
            return;
        }
        RegionFreeSlotList* freeSlotList = region->GetFreeSlotList();
        if (freeSlotList == nullptr || freeSlotList->GetMaxAvailableSize() < totalSize) {
            return;
        }
        uintptr_t addr = freeSlotList->AllocateSlot(totalSize, region->GetRegionStart());
        if (addr != 0) {
            CHECK_DETAIL(memset_s(reinterpret_cast<void*>(addr), totalSize, 0, totalSize) == EOK,
                         "zero free slot failed");
            result = static_cast<MAddress>(addr);
        }
    });
    return result;
}

MAddress GenerationAllocBuffer::AllocateOldOnNewRegion(MixedRegionManager &manager, size_t totalSize)
{
    CJThreadPreemptOffCntAdd();
    RegionInfo* r = manager.AllocateThreadLocalRegion();
    CJThreadPreemptOffCntSub();
    if (UNLIKELY(r == nullptr)) {
        LOG(RTLOG_FATAL, "promote object failed");
        return 0;
    }
    r->SetOldRegionFlag(1);
    if (oldAllocRegion == RegionInfo::NullRegion()) {
        oldAllocRegion = r;
        return r->Alloc(totalSize);
    }
    MAddress addr = oldAllocRegion->Alloc(totalSize);
    if (addr != 0) {
        manager.RemoveThreadLocalRegion(r);
        manager.ReclaimRegion(r);
        return addr;
    }
    manager.EnlistOldRegionList(oldAllocRegion);
    oldAllocRegion = r;
    return r->Alloc(totalSize);
}

} // namespace MapleRuntime
