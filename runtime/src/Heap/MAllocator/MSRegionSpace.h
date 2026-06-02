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
    MAddress Allocate(size_t size, AllocType allocType, ThreadLocalData* local);

    MAddress GetSpaceStartAddress() const override { return reservedStart; }
    MAddress GetSpaceEndAddress() const override;
    size_t GetCurrentCapacity() const override;
    size_t GetMaxCapacity() const override;
    size_t GetUsedPageSize() const override;
    size_t AllocatedBytes() const override;
    size_t LargeObjectBytes() const override;
    size_t ReclaimGarbageMemory(bool releaseAll) override;
    BackgroundReclaimStats BackgroundReclaim() override;
    bool ForEachObj(const std::function<void(BaseObject*)>& visitor, bool safe) const override;
    void FeedHungryBuffers() override;
    bool HasPendingReclaimWork() const override;

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool IsHeapObject(MAddress addr) const override;
#endif

    // MS-specific accessors
    CentralRegionPool* GetCentralPool() const { return msCentralPool_; }
    MSRegionManager* GetMSRegionManager() const { return msRegionManager_; }
    bool HasPendingSweepWork() const override;
    BackgroundSweepStats BackgroundSweep(uint8_t preferredCls) override;
    BackgroundFeedStats BackgroundFillAllocCaches(uint8_t preferredCls = kNoPreferredFeedClass) override;
    void EnsureThreadLocalAllocBuffer();

    // MS-specific metrics
    size_t GetRecentAllocatedSize() const;
    size_t GetSurvivedSize() const;
    size_t FromSpaceSize() const;
    size_t PinnedSpaceSize() const;

    // Scavenge-related queries (used by BackgroundScavengeLoop).
    // heapInUse    = active central-pool regions + live largeRegionList spans
    // heapRetained = active + emptySwept + largeHotCache + hotFree
    size_t GetHeapInUseBytes() const;
    // Swept-retained heap bytes = GetSweptMSRegionCount() * MS_REGION_SIZE.
    size_t GetSweptHeapBytes() const;
    size_t GetHeapRetainedBytes() const;
    size_t GetHeapThresholdBytes() const;
    size_t GetHotFreeBytes() const;
    bool HasEmptySweptRegions() const;
    // On-demand flush of the empty-swept cache → FreeManager hot-free supply.
    // All flush policy for both caches is centralized here; the actual release
    // is delegated to msCentralPool_->Reclaim* primitives.
    size_t FlushAllEmptySweptToHotFree();
    // Flush up to maxRegions empty-swept regions into FreeManager hot-free supply.
    size_t FlushEmptySweptToHotFree(size_t maxRegions);
    // On-demand flush of the large-hot-cache → FreeManager (releasedTree).
    size_t FlushAllLargeHotCache();
    // Flush up to maxBytes (largest entries first) of the large hot cache.
    size_t FlushLargeHotCacheUpToBytes(size_t maxBytes);
    // Step B: release up to quantumBytes of hot-free regions to OS. Returns bytes released.
    size_t BackgroundScavenge(size_t quantumBytes);
    // Step C: shrink committed heap end after scavenging.
    void BackgroundShrinkHeap();
    // True when heapRetained > heapThreshold (scavenge work remains).
    bool HasPendingScavengeWork() const;
    // Trigger scavenge when heapRetained exceeds heapThreshold.
    void MaybeNotifyScavenge();

    inline size_t GetTargetSize() const
    {
        double heapUtilization = CangjieRuntime::GetHeapParam().heapUtilization;
        return static_cast<size_t>(GetUsedPageSize() / heapUtilization);
    }

    friend class Allocator;

private:
    MAddress AllocateLargeObject(size_t size, size_t allocSize, ThreadLocalData* local);
    MAddress AllocateRegularObject(size_t size, size_t allocSize, ThreadLocalData* local);
    MAddress TryAllocateLargeObjectOnce(size_t allocSize, ThreadLocalData* local, AllocBufferMS* msBuffer);
    MAddress TryAllocateRegularObjectOnce(size_t allocSize, ThreadLocalData* local, AllocBufferMS*& msBuffer);
    void RegisterLargeObjectMS(MAddress addr, size_t regionBytes);

    MAddress reservedStart = 0;
    MAddress reservedEnd = 0;
    MemMap* map{ nullptr };
    CentralRegionPool* msCentralPool_ = nullptr;
    MSRegionManager* msRegionManager_ = nullptr;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_REGION_SPACE_H
