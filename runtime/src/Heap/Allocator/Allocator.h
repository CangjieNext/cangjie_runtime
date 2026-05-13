// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_ALLOCATOR_H
#define MRT_ALLOCATOR_H

#include "AllocBufferManager.h"
#include "Heap/GcThreadPool.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
constexpr uint8_t kNoPreferredFeedClass = UINT8_MAX;

struct BackgroundFeedStats {
    size_t buffersScanned = 0;
    size_t touchedBuffers = 0;
    size_t filledClasses = 0;
    size_t filledRegions = 0;
    size_t activeWorkingSetRegions = 0;
    uint8_t pressureLevel = 0;

    bool HasWork() const
    {
        return filledRegions != 0 || pressureLevel != 0;
    }

    void Merge(const BackgroundFeedStats& other)
    {
        buffersScanned = std::max(buffersScanned, other.buffersScanned);
        touchedBuffers += other.touchedBuffers;
        filledClasses += other.filledClasses;
        filledRegions += other.filledRegions;
        activeWorkingSetRegions = std::max(activeWorkingSetRegions, other.activeWorkingSetRegions);
        pressureLevel = std::max(pressureLevel, other.pressureLevel);
    }

    void Reset()
    {
        *this = BackgroundFeedStats{};
    }
};

// Allocator abstract class
class Allocator {
public:
    static constexpr size_t ALLOC_ALIGN = 8;
    static constexpr size_t HEADER_SIZE = 0; // no header for cangjie object

    static Allocator* NewAllocator();

    virtual MAddress Allocate(size_t size, AllocType allocType) = 0;
    virtual bool ForEachObj(const std::function<void(BaseObject*)>&, bool safe) const = 0;

    // release physical pages of garbage memory.
    virtual size_t ReclaimGarbageMemory(bool releaseAll) = 0;
#if defined(__EULER__)
    virtual void TryReclaimGarbageMemory() = 0;
#endif
    virtual void FeedHungryBuffers() = 0;
    virtual bool HasPendingReclaimWork() const { return false; }
    virtual bool IsReclaimPressureHigh() const { return false; }
    virtual bool HasPendingSweepWork() const { return false; }
    virtual void BackgroundSweep(uint8_t preferredCls) { (void)preferredCls; }
    virtual BackgroundFeedStats BackgroundFillAllocCaches(uint8_t preferredCls = UINT8_MAX)
    {
        (void)preferredCls;
        return {};
    }

    // returns the total size of live large objects, excluding alignment/roundup/header, ...
    // LargeObjects() is missing.
    virtual size_t LargeObjectBytes() const = 0;

    // returns the total bytes that has been allocated, including alignment/roundup/header, ...
    // allocated bytes for large objects are included.
    virtual size_t AllocatedBytes() const = 0;



    virtual ~Allocator() {}
    Allocator();

    virtual void Init(const HeapParam&) = 0;
    virtual size_t GetMaxCapacity() const = 0;
    virtual size_t GetCurrentCapacity() const = 0;
    virtual size_t GetUsedPageSize() const = 0;
    virtual MAddress GetSpaceStartAddress() const = 0;
    virtual MAddress GetSpaceEndAddress() const = 0;
    // IsHeapAddress is a range-based check, used to quickly identify heap address,
    // non-heap address never falls into this address range.
    // for more accurate check, use IsHeapObject().
    ALWAYS_INLINE bool IsHeapAddress(MAddress addr) const { return Heap::IsHeapAddress(addr); }

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    virtual bool IsHeapObject(MAddress) const = 0;
#endif

    // GC mark/enqueue interface — implementations in RegionSpace.cpp (CopyGC) / MSRegionSpace.cpp (MS)
    static bool MarkObject(const BaseObject* obj);
    static bool IsMarkedObject(const BaseObject* obj);
    static bool ShouldEnqueue(const BaseObject* obj);
    static bool IsResurrectedObject(const BaseObject* obj);

    // Thread-local buffer interface — implementations in RegionSpace.cpp (CopyGC) / MSRegionSpace.cpp (MS)
    static void EnsureThreadLocalBuffer();
    static void CommitRawPointerRegions();
    static void PushRootToBuffer(BaseObject* root);
};
} // namespace MapleRuntime
#endif // MRT_ALLOCATOR_H
