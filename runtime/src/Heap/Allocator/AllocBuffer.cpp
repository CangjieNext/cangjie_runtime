// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/AllocBuffer.h"
#include "Allocator/RegionSpace.h"
#include "Collector/FinalizerProcessor.h"
#include "Heap.h"

namespace MapleRuntime {

AllocBuffer* AllocBuffer::GetOrCreateAllocBuffer()
{
    auto* buffer = AllocBuffer::GetAllocBuffer();
    if (buffer == nullptr) {
        buffer = new (std::nothrow) AllocBuffer();
        CHECK_DETAIL(buffer != nullptr, "new region alloc buffer fail");
        buffer->Init();
        ThreadLocal::SetAllocBuffer(buffer);
    }
    return buffer;
}

AllocBuffer* AllocBuffer::GetAllocBuffer() { return ThreadLocal::GetAllocBuffer(); }

AllocBuffer::~AllocBuffer()
{
    if (LIKELY(tlRegion != RegionInfo::NullRegion()) && tlRegion != nullptr) {
        RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
        RegionManager& manager = theAllocator.GetRegionManager();
        manager.RemoveThreadLocalRegion(tlRegion);
        manager.EnlistFullThreadLocalRegion(tlRegion);
        tlRegion = RegionInfo::NullRegion();
    }
}

void AllocBuffer::Init()
{
    static_assert(offsetof(AllocBuffer, tlRegion) == 0,
                  "need to modify the offset of this value in llvm-project at the same time");
    tlRegion = RegionInfo::NullRegion();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.RegisterAllocBuffer(*this);
}

void AllocBuffer::Fini()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    space.RemoveAllocBuffer(*this);
}

MAddress AllocBuffer::Allocate(size_t totalSize, AllocType allocType)
{
    // a hoisted specific fast path which can be inlined
    MAddress addr = 0;
    if (UNLIKELY(allocType == AllocType::RAW_POINTER_OBJECT)) {
        return AllocateRawPointerObject(totalSize);
    }

    if (LIKELY(tlRegion != RegionInfo::NullRegion())) {
        addr = tlRegion->Alloc(totalSize);
    }

    if (UNLIKELY(addr == 0)) {
        addr = AllocateImpl(totalSize, allocType);
    }

    DLOG(ALLOC, "alloc 0x%zx(%zu)", addr, totalSize);
    return addr;
}

// try an allocation but do not handle failure
MAddress AllocBuffer::AllocateImpl(size_t totalSize, AllocType allocType)
{
    RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& manager = theAllocator.GetRegionManager();

    // allocate from thread local region
    if (LIKELY(tlRegion != RegionInfo::NullRegion())) {
        MAddress addr = tlRegion->Alloc(totalSize);
        if (addr != 0) {
            return addr;
        }

        // allocation failed because region is full.
        CHECK(tlRegion->IsThreadLocalRegion());
        {
            manager.RemoveThreadLocalRegion(tlRegion);
            manager.EnlistFullThreadLocalRegion(tlRegion);
            tlRegion = RegionInfo::NullRegion();
        }
    }

    // now region must be null. If a region has been ready, then use it and tell gc-assitant thread to prepare
    // a new region, or take a new one.
    RegionInfo* r  = preparedRegion.load(std::memory_order_acquire);
    if (r != nullptr) {
        preparedRegion.store(nullptr, std::memory_order_release);
        tlRegion = r;
        theAllocator.AddHungryBuffer(*this);
        Heap::GetHeap().GetFinalizerProcessor().NotifyToFeedAllocBuffers();
        return r->Alloc(totalSize);
    }
    // AllocateThreadLocalRegion is a safepoint, in which cj thread rescheule may happen.
    // tlRegion is bound to specific thread, so we need to forbid reschedule.
    CJThreadPreemptOffCntAdd();
    r = manager.AllocateThreadLocalRegion();
    CJThreadPreemptOffCntSub();
    if (UNLIKELY(r == nullptr)) {
        return 0;
    }
    // tlRegion may be set in PreforwardPhase handler while allocating region.
    // Null region means tlRegion is not set.
    if (tlRegion == RegionInfo::NullRegion()) {
        tlRegion = r;
        return r->Alloc(totalSize);
    }
    // tlRegion has been set in preforward phase.
    MAddress addr = tlRegion->Alloc(totalSize);
    if (addr != 0) {
        if (!SetPreparedRegion(r)) {
            // r is inserted in thread-local region list when allocated, we need to remove it from the list.
            manager.RemoveThreadLocalRegion(r);
            manager.ReclaimRegion(r);
        }
        return addr;
    }
    // tlRegion is not enough for allocation, so we use r.
    manager.RemoveThreadLocalRegion(tlRegion);
    manager.EnlistFullThreadLocalRegion(tlRegion);
    tlRegion = r;
    return r->Alloc(totalSize);
}

MAddress AllocBuffer::AllocateRawPointerObject(size_t totalSize)
{
    RegionInfo* region = tlRawPointerRegions.GetHeadRegion();
    if (region != nullptr) {
        MAddress allocAddr = region->Alloc(totalSize);
        if (allocAddr != 0) {
            return allocAddr;
        }
    }
    RegionManager& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    size_t needUnitNum = AlignUp(totalSize, RegionInfo::UNIT_SIZE) / RegionInfo::UNIT_SIZE;
    if (totalSize <= manager.GetThreadLocalRegionSize()) {
        region = manager.TakeRegion(needUnitNum, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        if (region == nullptr) {
            return 0;
        }
        tlRawPointerRegions.PrependRegion(region, RegionInfo::RegionType::TL_RAW_POINTER_REGION);
    } else {
        region = manager.TakeRegion(needUnitNum, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
        if (region == nullptr) {
            return 0;
        }
        tlLargeRawPointerRegions.PrependRegion(region, RegionInfo::RegionType::TL_LARGE_RAW_POINTER_REGION);
    }

    // region is enough for totalSize.
    MAddress allocAddr = region->Alloc(totalSize);
    MRT_ASSERT(allocAddr != 0, "allocation failure");
    return allocAddr;
}

void AllocBuffer::CommitRawPointerRegions()
{
    RegionManager& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    manager.MergeRawPointerRegions(tlRawPointerRegions, tlLargeRawPointerRegions);
}

} // namespace MapleRuntime
