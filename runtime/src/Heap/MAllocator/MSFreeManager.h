// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_FREE_MANAGER_H
#define MRT_MS_FREE_MANAGER_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "Base/Panic.h"

namespace MapleRuntime {

class FreeManager {
public:
    using UnitIndex = uint32_t;
    using UnitCount = uint32_t;
    using RegionIndex = uint32_t;
    using RegionCount = uint32_t;

    struct UnitSpan {
        UnitIndex idx = 0;
        UnitCount num = 0;
    };

    struct RegionSpan {
        RegionIndex idx = 0;
        RegionCount num = 0;
    };

    struct FinalStats {
        size_t committedRegions = 0;
        size_t committedBytes = 0;
        size_t freeRegions = 0;
        size_t freeBytes = 0;
        size_t hotFreeRegions = 0;
        size_t hotFreeBytes = 0;
        size_t scavengedRegions = 0;
        size_t scavengedBytes = 0;
        size_t usedRegions = 0;
        size_t usedBytes = 0;
        size_t metadataBytes = 0;
        uint32_t maxFreeRunRegions = 0;
        uint64_t heapGrowCount = 0;
    };

    enum class TryTakeResult : uint8_t {
        SUCCESS,
        EMPTY,
        BUSY,
    };

    class HeapRangeHandler {
    public:
        virtual ~HeapRangeHandler() = default;

        virtual void InitializeFreshRegions(RegionIndex startIdx, RegionCount count) = 0;
        virtual void OnCommittedHeapExtended(uintptr_t newCommittedEnd) = 0;
    };

    FreeManager() = default;
    ~FreeManager();

    void Initialize(UnitCount maxUnitCount, size_t unitSize, size_t unitsPerRegion,
                    RegionCount committedRegionCount, RegionCount maxVirtualRegionCount,
                    uintptr_t heapStart, uintptr_t committedEnd, uintptr_t maxHeapEnd,
                    size_t segmentSize);

    TryTakeResult TryTakeRegions(RegionCount num, RegionIndex& idx);
    void RecordTakeBusyFallback() {}
    bool TryIncreaseInactiveZone(RegionCount num, RegionIndex& idx);
    void ReclaimRegionsReleased(RegionIndex idx, RegionCount num);
    bool GrowOneSegment(HeapRangeHandler& handler);
    void ShrinkHeap();
    size_t ScavengeFreeRegions(bool releaseAll);

    UnitCount GetReleasedUnitCount() const;
    UnitCount GetFreeUnitCount() const;
    size_t GetReleasedBytes() const;
    uint32_t GetNodeCount() const { return 0; }
    uint32_t GetMaxBlockRegions() const;
    size_t GetMetadataBytesInUse() const;
    size_t GetNodeBytesInUse() const { return 0; }
    FinalStats GetFinalStats() const;
    void CollectRegionSpans(std::vector<RegionSpan>& spans) const;

    uintptr_t GetCommittedEnd() const { return committedEnd_; }
    uintptr_t GetMaxHeapEnd() const { return maxHeapEnd_; }
    uintptr_t GetInactiveZone() const { return committedEnd_; }
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
    struct ChunkSummary {
        uint16_t start = 0;
        uint16_t max = 0;
        uint16_t end = 0;
    };

    static constexpr size_t kChunkBits = 64;

    static bool TestBit(const uint64_t* bits, size_t idx)
    {
        return (bits[idx / kChunkBits] >> (idx % kChunkBits)) & 1ULL;
    }

    static void SetBit(uint64_t* bits, size_t idx)
    {
        bits[idx / kChunkBits] |= (1ULL << (idx % kChunkBits));
    }

    static void ClearBit(uint64_t* bits, size_t idx)
    {
        bits[idx / kChunkBits] &= ~(1ULL << (idx % kChunkBits));
    }

    static size_t RegionCountToChunkCount(size_t regionCount)
    {
        return (regionCount + kChunkBits - 1) / kChunkBits;
    }

    bool IsRegionFreeLocked(RegionIndex idx) const;
    bool IsRegionScavengedLocked(RegionIndex idx) const;
    RegionIndex FindNextFreeRegionLocked(RegionIndex start) const;
    bool PreciseFitFromLocked(RegionIndex start, RegionCount num) const;
    bool FindFreeRunLocked(RegionCount num, RegionIndex& idx) const;
    size_t CountScavengedInRangeLocked(RegionIndex idx, RegionCount num) const;
    size_t CountFreeInRangeLocked(RegionIndex idx, RegionCount num) const;
    void MarkAllocatedLocked(RegionIndex idx, RegionCount num, bool& reusedScavenged);
    void MarkFreeLocked(RegionIndex idx, RegionCount num);
    void MarkScavengedLocked(RegionIndex idx, RegionCount num);
    void MarkUnavailableLocked(RegionIndex idx, RegionCount num);
    void UpdateChunkSummaryLocked(size_t chunkIdx);
    void UpdateChunkSummaryRangeLocked(RegionIndex idx, RegionCount num);
    size_t CountTrailingFreeRegionsLocked() const;
    void ReleaseUnits(UnitIndex idx, UnitCount num) const;
    void ReuseUnits(UnitIndex idx, UnitCount num) const;
    void CheckAlignmentConfig() const;
    size_t GetRegionSizeBytes() const;
    RegionIndex AddrToRegionIdx(uintptr_t addr) const;
    uintptr_t RegionIdxToAddr(RegionIndex regionIdx) const;
    void ReserveMetadataStorage(size_t bitmapWordCapacity, size_t summaryChunkCapacity);
    void ReleaseMetadataStorage();
    void EnsureMetadataInitializedForRegionCount(size_t regionCount);

    mutable std::mutex pageHeapMutex_;
    mutable std::mutex growMutex_;
    uint64_t* allocBits_ = nullptr;
    uint64_t* scavengedBits_ = nullptr;
    ChunkSummary* chunkSummaries_ = nullptr;
    size_t bitmapWordCapacity_ = 0;
    size_t bitmapWordCountInUse_ = 0;
    size_t summaryChunkCapacity_ = 0;
    size_t summaryChunkCountInUse_ = 0;
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
    size_t freeRegionCount_ = 0;
    size_t scavengedRegionCount_ = 0;
    RegionIndex searchIdx_ = 0;
    uint64_t heapGrowCount_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_FREE_MANAGER_H