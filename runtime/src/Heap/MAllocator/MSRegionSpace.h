// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MS_REGION_SPACE_H
#define MRT_MS_REGION_SPACE_H

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/Allocator/AllocUtil.h"
#include "Heap/Allocator/Allocator.h"
#include "Heap/MAllocator/MSFreeManager.h"
#include "Heap/MAllocator/MSScanClass.h"

namespace MapleRuntime {

// Forward declarations
class CentralRegionPool;
class MSRegionManager;
class AllocBufferMS;
class Mutator;
class MemMap;
struct ThreadLocalData;

// RegionSpaceMS is the Allocator implementation for Mark-Sweep GC mode.
// It manages memory via MSRegionManager/CentralRegionPool,
// and uses AllocBufferMS for thread-local allocation.
class RegionSpaceMS : public Allocator {
public:
    static size_t ToAllocSize(size_t objSize)
    {
        size_t size = objSize + HEADER_SIZE;
        return RoundUp<size_t>(size, ALLOC_ALIGN);
    }

    static size_t GetAllocSize(const BaseObject& obj)
    {
        size_t objSize = obj.GetSize();
        return ToAllocSize(objSize);
    }

    RegionSpaceMS() = default;
    ATTR_NO_INLINE ~RegionSpaceMS() override;

    void Init(const HeapParam&) override;
    MAddress Allocate(size_t size, AllocType allocType) override;
    MAddress Allocate(size_t size, MSScanClass scanClass,
                      ThreadLocalData* local);

    MAddress GetSpaceStartAddress() const override { return reservedStart; }
    MAddress GetSpaceEndAddress() const override;
    size_t GetCurrentCapacity() const override;
    size_t GetMaxCapacity() const override;
    size_t GetUsedPageSize() const override;
    size_t AllocatedBytes() const override;
    size_t LargeObjectBytes() const override;
    bool ForEachObj(const std::function<void(BaseObject*)>& visitor, bool safe) const override;
    void FeedHungryBuffers() override;

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool IsHeapObject(MAddress addr) const override;
#endif

    // MS-specific accessors
    CentralRegionPool* GetCentralPool() const { return msCentralPool_; }
    MSRegionManager* GetMSRegionManager() const { return msRegionManager_; }
    // Batch-flush the empty-swept cache into hot-free until the remaining cache
    // footprint is <= thresholdBytes.
    size_t FlushCentralCacheToThreshold(size_t thresholdBytes);
    // Step B: release up to quantumBytes of hot-free regions to OS. Returns bytes released.
    size_t BackgroundScavenge(size_t quantumBytes);
    // Release one fully-idle segment worth of pages while keeping the current
    // committed/owned-segment topology intact. Returns newly released bytes.
    size_t BackgroundShrinkOneSegment();
    // True when at least one segment has no in-use regions and still holds
    // unreleased physical pages or central-pool cached regions.
    bool HasPendingShrinkWork() const;
    friend class Allocator;

private:
    MAddress AllocateInternal(
        size_t size, MSScanClass scanClass, ThreadLocalData* local);
    MAddress AllocateLargeObject(
        size_t size, size_t allocSize, MSScanClass scanClass, ThreadLocalData* local);
    MAddress AllocateRegularObject(
        size_t size, size_t allocSize, MSScanClass scanClass, ThreadLocalData* local);
    MAddress TryAllocateLargeObjectOnce(size_t objectBytes, size_t allocSize,
                                        MSScanClass scanClass, ThreadLocalData* local,
                                        AllocBufferMS* msBuffer);
    MAddress TryAllocateRegularObjectOnce(size_t allocSize, MSScanClass scanClass,
                                          ThreadLocalData* local, AllocBufferMS*& msBuffer);
    void RegisterLargeObjectMS(
        MAddress addr, size_t objectBytes, size_t regionBytes, MSScanClass scanClass);

    MAddress reservedStart = 0;
    MAddress reservedEnd = 0;
    MemMap* map{ nullptr };
    CentralRegionPool* msCentralPool_ = nullptr;
    MSRegionManager* msRegionManager_ = nullptr;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_REGION_SPACE_H
