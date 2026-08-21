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
MAddress GenerationAllocBuffer::AllocateImpl(size_t totalSize)
{
    MixedRegionSpace& theAllocator = MixedRegionSpace::CastDown(Heap::GetHeap().GetAllocator());
    MixedRegionManager& manager = MixedRegionSpace::CastDown(theAllocator.GetRegionManager());

    if (LIKELY(tlRegion != RegionInfo::NullRegion())) {
        MAddress addr = tlRegion->Alloc(totalSize);
        if (addr != 0) {
            return addr;
        }

        CHECK(tlRegion->IsThreadLocalRegion());
        {
            manager.RemoveThreadLocalRegion(tlRegion);
            manager.EnlistFullThreadLocalRegion(tlRegion);
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
    MAddress result = 0;
    manager.GetSweptRegionList().VisitAllRegions([&totalSize, &result](RegionInfo* region) {
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
