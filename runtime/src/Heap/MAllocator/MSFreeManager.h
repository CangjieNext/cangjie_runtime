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
#include <deque>
#include <mutex>
#include <vector>

#include "Base/Panic.h"
#include "Heap/MAllocator/MSShardRouting.h"

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

    struct BorrowedSpan {
        uint16_t ownerShardId = 0;
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
    ~FreeManager() = default;

    void Initialize(UnitCount maxUnitCount, size_t unitSize, size_t unitsPerRegion,
                    RegionCount committedRegionCount, RegionCount maxVirtualRegionCount,
                    uintptr_t heapStart, uintptr_t committedEnd, uintptr_t maxHeapEnd,
                    size_t segmentSize, uint32_t shardCount);

    TryTakeResult TryTakeRegions(RegionCount num, uint16_t homeShardId, RegionIndex& idx);
    void RecordTakeBusyFallback() {}
    bool TryIncreaseInactiveZone(RegionCount num, RegionIndex& idx);
    void ReclaimRegionsReleased(RegionIndex idx, RegionCount num);
    void ReclaimRegionsReleasedBatch(const std::vector<RegionSpan>& spans);
    bool GrowOneSegment(uint16_t ownerShardId, HeapRangeHandler& handler);
    void ShrinkHeap();
    size_t ScavengeFreeRegions(bool releaseAll);

    UnitCount GetReleasedUnitCount() const;
    UnitCount GetFreeUnitCount() const;
    size_t GetReleasedBytes() const;
    size_t GetOwnedFreeRegionCount(uint16_t ownerShardId) const;
    size_t GetScavengedRegionCount(uint16_t ownerShardId) const;
    size_t GetHotFreeRegionCount(uint16_t ownerShardId) const;
    size_t GetBorrowedRegionCount(uint16_t receiverShardId) const;
    size_t GetOwnedSegmentCount(uint16_t ownerShardId) const;
    RegionIndex GetShardTakeCursor(uint16_t ownerShardId) const;
    RegionIndex GetShardReclaimCursor(uint16_t ownerShardId) const;
    uint32_t GetNodeCount() const { return 0; }
    uint32_t GetMaxBlockRegions() const;
    size_t GetMetadataBytesInUse() const;
    size_t GetNodeBytesInUse() const { return 0; }
    FinalStats GetFinalStats() const;
    void CollectRegionSpans(std::vector<RegionSpan>& spans) const;

    // Return bytes in free regions whose physical pages have NOT been returned
    // to the OS yet (hotFree = freeRegions - scavengedRegions).
    size_t GetHotFreeBytes() const;

    // Like ScavengeFreeRegions, but caps the work at maxBytes.
    // Returns actual bytes released (madvise'd).
    size_t ScavengeFreeRegionsUpTo(size_t maxBytes);

    uintptr_t GetCommittedEnd() const { return committedEnd_; }
    uintptr_t GetMaxHeapEnd() const { return maxHeapEnd_; }
    uintptr_t GetInactiveZone() const { return committedEnd_; }
    size_t GetCurrentCapacityBytes() const { return committedEnd_ - heapStart_; }
    size_t GetCommittedRegionCount() const { return committedRegionCount_; }
    size_t GetCommittedUnitCount() const { return committedUnitCount_; }
    size_t GetMaxVirtualRegionCount() const { return maxVirtualRegionCount_; }
    size_t GetMaxVirtualUnitCount() const { return maxVirtualUnitCount_; }
    uint64_t GetHeapGrowCount() const { return heapGrowCount_; }
    uint16_t ResolveOwnerShardId(RegionIndex idx) const;

    UnitIndex RegionToUnitIdx(RegionIndex regionIdx) const;
    UnitCount RegionCountToUnitCount(RegionCount regionCount) const;
    RegionIndex UnitToRegionIdx(UnitIndex unitIdx) const;
    RegionCount UnitCountToRegionCount(UnitCount unitCount) const;

private:
    static constexpr RegionCount kSingleRegionStealBatch = 4;
    static constexpr uint32_t kMaxSingleRegionStealAttempts = 2;

    struct ChunkSummary {
        uint16_t start = 0;
        uint16_t max = 0;
        uint16_t end = 0;
    };

    struct OwnedSegmentView {
        size_t segmentIdx_ = 0;
        RegionIndex regionStart_ = 0;
        RegionIndex regionEnd_ = 0;
        size_t bitmapWordCount_ = 0;
        size_t summaryChunkCount_ = 0;
        std::vector<uint64_t> allocBits_;
        std::vector<uint64_t> scavengedBits_;
        std::vector<ChunkSummary> chunkSummaries_;
    };

    struct FreeManagerShard {
        mutable std::mutex shardMutex_;
        size_t freeRegionCount_ = 0;
        size_t scavengedRegionCount_ = 0;
        RegionIndex takeCursor_ = 0;
        RegionIndex reclaimCursor_ = 0;
        uint16_t nextStealDonorShard_ = 0;
        size_t borrowedRegionCount_ = 0;
        std::vector<size_t> ownedSegments_;
        std::vector<OwnedSegmentView> ownedViews_;
        std::deque<BorrowedSpan> borrowedSpans_;
        size_t bitmapWordCountInUse_ = 0;
        size_t summaryChunkCountInUse_ = 0;
        uint32_t maxOwnedFreeRun_ = 0;
        std::atomic<uint32_t> snapMaxFreeRun_{0};
        std::atomic<uint32_t> snapHotFreeRegions_{0};
        bool fastTakeHintValid_ = false;
        size_t fastTakeViewIdx_ = 0;
        size_t fastTakeWordOffset_ = 0;
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

    OwnedSegmentView BuildOwnedSegmentView(size_t segmentIdx, RegionCount regionCount) const;
    void AttachOwnedSegmentLocked(FreeManagerShard& shard, size_t segmentIdx, RegionCount regionCount);
    void DetachOwnedSegmentLocked(FreeManagerShard& shard);
    FreeManagerShard& GetShard(uint16_t ownerShardId);
    const FreeManagerShard& GetShard(uint16_t ownerShardId) const;
    OwnedSegmentView* FindOwnedSegmentViewBySegmentIdxLocked(FreeManagerShard& shard, size_t segmentIdx);
    const OwnedSegmentView* FindOwnedSegmentViewBySegmentIdxLocked(const FreeManagerShard& shard, size_t segmentIdx) const;
    OwnedSegmentView* FindOwnedSegmentViewByRegionLocked(FreeManagerShard& shard, RegionIndex idx);
    const OwnedSegmentView* FindOwnedSegmentViewByRegionLocked(const FreeManagerShard& shard, RegionIndex idx) const;
    bool IsRegionFreeLocked(RegionIndex idx) const;
    bool IsRegionScavengedLocked(RegionIndex idx) const;
    uint16_t ResolveOwnerShardIdLocked(RegionIndex idx) const;
    RegionIndex FindNextFreeRegionLocked(RegionIndex start) const;
    RegionIndex FindNextOwnedFreeRegionLocked(const FreeManagerShard& shard, RegionIndex start) const;
    RegionIndex FindFirstFreeRegionInViewLocked(const OwnedSegmentView& view, RegionIndex start) const;
    bool PreciseFitFromLocked(RegionIndex start, RegionCount num) const;
    bool TryTakeBorrowedRegionLocked(FreeManagerShard& shard, RegionIndex& idx);
    bool FindFreeRunInRangeLocked(RegionIndex start, RegionIndex end, RegionCount num, RegionIndex& idx) const;
    bool FindFreeRunLocked(RegionIndex cursor, RegionCount num, RegionIndex& idx) const;
    bool FindOwnedFreeRunLocked(const FreeManagerShard& shard, RegionIndex cursor, RegionCount num, RegionIndex& idx) const;
    RegionCount CountContiguousFreeRegionsLocked(RegionIndex start, RegionIndex end, RegionCount maxCount) const;
    size_t ScavengeOwnedFreeRegionsLocked(FreeManagerShard& shard, uint16_t ownerShardId, size_t regionBudget);
    void AppendGroupedOwnerSpansLocked(std::vector<std::vector<RegionSpan>>& groupedSpans,
                                       RegionIndex idx,
                                       RegionCount num) const;
    void ApplyGroupedOwnerReclaim(std::vector<std::vector<RegionSpan>>& groupedSpans);
    bool TryTakeLocalRegionsLocked(FreeManagerShard& shard, RegionCount num, RegionIndex& idx, bool& reusedScavenged);
    bool TryTakeOneRegionFastLocked(FreeManagerShard& shard, RegionIndex& idx, bool& reusedScavenged);
    bool TryTakeOneRegionFromOwnedViewsLocked(FreeManagerShard& shard,
                                              size_t startViewIdx,
                                              RegionIndex startRegion,
                                              RegionIndex& idx,
                                              bool& reusedScavenged);
    void MarkAllocatedOneLocked(FreeManagerShard& shard,
                                OwnedSegmentView& view,
                                size_t localIdx,
                                bool& reusedScavenged);
    bool TryStealSingleRegionBatchLocked(uint16_t receiverShardId, RegionIndex& idx, RegionSpan& reusedSpan);
    bool TryStealOwnedRunLocked(uint16_t receiverShardId, RegionCount num, RegionIndex& idx, RegionSpan& reusedSpan);
    bool SelectBestDonorShardForRun(uint16_t receiverShardId,
                                    uint16_t startDonorShardId,
                                    RegionCount num,
                                    uint16_t& donorShardId);
    size_t CountScavengedInViewRangeLocked(const OwnedSegmentView& view, size_t localStart, RegionCount num) const;
    size_t CountFreeInViewRangeLocked(const OwnedSegmentView& view, size_t localStart, RegionCount num) const;
    void MarkAllocatedLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num, bool& reusedScavenged);
    void MarkFreeLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num);
    void MarkScavengedLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num);
    void MarkUnavailableLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num);
    void UpdateChunkSummaryLocked(OwnedSegmentView& view, size_t chunkIdx);
    void UpdateChunkSummaryRangeLocked(OwnedSegmentView& view, size_t localStart, RegionCount num);
    size_t CountTrailingFreeRegionsLocked() const;
    void ReleaseUnits(UnitIndex idx, UnitCount num) const;
    void ReuseUnits(UnitIndex idx, UnitCount num) const;
    void CheckAlignmentConfig() const;
    size_t GetRegionSizeBytes() const;
    RegionIndex AddrToRegionIdx(uintptr_t addr) const;
    uintptr_t RegionIdxToAddr(RegionIndex regionIdx) const;
    uint32_t ComputeMaxFreeRunInViewLocked(const OwnedSegmentView& view) const;
    void RefreshShardMaxOwnedFreeRunLocked(FreeManagerShard& shard);

    mutable std::mutex pageHeapMutex_;
    mutable std::mutex growMutex_;
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
    uint32_t shardCount_ = 1;
    size_t segmentRegionCount_ = 0;
    uint64_t heapGrowCount_ = 0;
    std::deque<FreeManagerShard> shards_;
    std::vector<uint16_t> segmentOwnerShard_;
    uint16_t nextScavengeShard_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_FREE_MANAGER_H
