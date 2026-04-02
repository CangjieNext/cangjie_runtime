// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_FREE_MANAGER_H
#define MRT_MS_FREE_MANAGER_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "Heap/Allocator/CartesianTree.h"
#include "Base/Panic.h"

namespace MapleRuntime {

class FreeManager {
public:
    using UnitIndex = CartesianTree::Index;
    using UnitCount = CartesianTree::Count;
    using RegionIndex = CartesianTree::Index;
    using RegionCount = CartesianTree::Count;

    struct UnitSpan {
        UnitIndex idx = 0;
        UnitCount num = 0;
    };

    struct RegionSpan {
        RegionIndex idx = 0;
        RegionCount num = 0;
    };

    enum class TryTakeResult : uint8_t {
        SUCCESS,
        EMPTY,
        BUSY,
    };

    class Backend {
    public:
        virtual ~Backend() = default;

        virtual void Initialize(UnitCount maxUnitCount) = 0;
        virtual void Finalize() noexcept = 0;

        virtual bool TakeUnits(UnitCount num, UnitIndex& idx) = 0;
        virtual bool TakeTrailingUnits(UnitIndex targetEnd, UnitIndex& idx, UnitCount& num) = 0;
        virtual bool MergeInsert(UnitIndex idx, UnitCount num) = 0;

        virtual UnitCount GetTotalCount() const = 0;
        virtual uint32_t GetNodeCount() const = 0;
        virtual uint32_t GetMaxBlockCount() const = 0;
        virtual size_t GetMetadataBytesInUse() const = 0;
        virtual size_t GetNodeBytesInUse() const = 0;
        virtual void CollectUnitSpans(std::vector<UnitSpan>& spans) const = 0;
    };

    class HeapRangeHandler {
    public:
        virtual ~HeapRangeHandler() = default;

        virtual void InitializeFreshRegions(RegionIndex startIdx, RegionCount count) = 0;
        virtual void OnCommittedHeapExtended(uintptr_t newCommittedEnd) = 0;
    };

    FreeManager();
    explicit FreeManager(std::unique_ptr<Backend> backend);
    ~FreeManager();

    void Initialize(UnitCount maxUnitCount, size_t unitSize, size_t unitsPerRegion,
                    RegionCount committedRegionCount, RegionCount maxVirtualRegionCount,
                    uintptr_t heapStart, uintptr_t committedEnd, uintptr_t maxHeapEnd,
                    size_t segmentSize);

    TryTakeResult TryTakeRegions(RegionCount num, RegionIndex& idx);
    bool TryIncreaseInactiveZone(RegionCount num, RegionIndex& idx);
    void ReclaimRegionsReleased(RegionIndex idx, RegionCount num);
    bool GrowOneSegment(HeapRangeHandler& handler);
    void ShrinkHeap();

    UnitCount GetReleasedUnitCount() const;
    UnitCount GetFreeUnitCount() const;
    size_t GetReleasedBytes() const;
    uint32_t GetNodeCount() const;
    uint32_t GetMaxBlockRegions() const;
    size_t GetMetadataBytesInUse() const;
    size_t GetNodeBytesInUse() const;
    void CollectRegionSpans(std::vector<RegionSpan>& spans) const;

    uintptr_t GetCommittedEnd() const { return committedEnd_; }
    uintptr_t GetMaxHeapEnd() const { return maxHeapEnd_; }
    uintptr_t GetInactiveZone() const { return inactiveZone_.load(std::memory_order_relaxed); }
    size_t GetCurrentCapacityBytes() const { return committedEnd_ - heapStart_; }
    size_t GetCommittedRegionCount() const { return committedRegionCount_; }
    size_t GetCommittedUnitCount() const { return committedUnitCount_; }
    size_t GetMaxVirtualRegionCount() const { return maxVirtualRegionCount_; }
    size_t GetMaxVirtualUnitCount() const { return maxVirtualUnitCount_; }
    uint64_t GetHeapGrowCount() const { return heapGrowCount_; }

    UnitIndex RegionToUnitIdx(RegionIndex regionIdx) const;
    UnitCount RegionCountToUnitCount(RegionCount regionCount) const;
    RegionIndex UnitToRegionIdx(UnitIndex unitIdx) const;
    RegionCount UnitCountToRegionCount(UnitCount unitCount) const;

private:
    bool TakeTrailingRegions(RegionIndex targetEnd, RegionIndex& idx, RegionCount& num);
    void ReleaseUnits(UnitIndex idx, UnitCount num) const;
    void InsertRegions(RegionIndex idx, RegionCount num);
    Backend& GetBackend();
    const Backend& GetBackend() const;
    void CheckAlignmentConfig() const;
    size_t GetRegionSizeBytes() const;
    RegionIndex AddrToRegionIdx(uintptr_t addr) const;
    uintptr_t RegionIdxToAddr(RegionIndex regionIdx) const;
    size_t RewindInactiveZoneUsingReleasedTail();

    mutable std::mutex mutex_;
    std::unique_ptr<Backend> backend_;
    size_t unitSize_ = 0;
    size_t unitsPerRegion_ = 0;
    uintptr_t heapStart_ = 0;
    uintptr_t maxHeapEnd_ = 0;
    uintptr_t committedEnd_ = 0;
    size_t committedRegionCount_ = 0;
    size_t committedUnitCount_ = 0;
    size_t maxVirtualRegionCount_ = 0;
    size_t maxVirtualUnitCount_ = 0;
    size_t segmentSize_ = 0;
    std::atomic<uintptr_t> inactiveZone_{0};
    mutable std::mutex growMutex_;
    uint64_t heapGrowCount_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_FREE_MANAGER_H