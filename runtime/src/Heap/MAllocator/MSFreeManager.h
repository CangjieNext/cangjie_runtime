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
#include <deque>
#include <memory>
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

    struct RegionUsageSnapshot {
        size_t committedRegions = 0;
        size_t freeRegions = 0;
    };

    struct BorrowedSpan {
        uint16_t ownerShardId = 0;
        RegionIndex idx = 0;
        RegionCount num = 0;
    };

    struct SegmentFreeSnapshot {
        size_t validRegions = 0;
        size_t freeRegions = 0;
        size_t scavengedRegions = 0;
    };

    enum class TryTakeResult : uint8_t {
        SUCCESS,
        EMPTY,
    };

    class HeapRangeHandler {
    public:
        virtual ~HeapRangeHandler() = default;

        virtual void InitializeFreshRegions(RegionIndex startIdx, RegionCount count) = 0;
        virtual void OnCommittedHeapExtended(uintptr_t newCommittedEnd) = 0;
    };

    FreeManager() = default;
    ~FreeManager() = default;

    void Initialize(size_t unitSize, size_t unitsPerRegion, RegionCount committedRegionCount,
                    RegionCount maxVirtualRegionCount,
                    uintptr_t heapStart, uintptr_t committedEnd, uintptr_t maxHeapEnd,
                    size_t segmentSize, uint32_t shardCount);

    TryTakeResult TryTakeRegions(RegionCount num, uint16_t homeShardId, RegionIndex& idx);
    bool TryTakeOrGrowRegions(RegionCount num,
                              uint16_t homeShardId,
                              RegionIndex& idx,
                              HeapRangeHandler& handler);
    void ReclaimRegionsReleased(RegionIndex idx, RegionCount num);
    void ReclaimRegionsReleasedBatch(const std::vector<RegionSpan>& spans);
    bool GrowOneSegment(uint16_t ownerShardId, HeapRangeHandler& handler);
    bool GrowForRegions(RegionCount numRegions, uint16_t ownerShardId, HeapRangeHandler& handler);
    void ShrinkHeap();
    size_t ScavengeFreeRegions(bool releaseAll);

    uint32_t GetShardCount() const { return shardCount_; }
    UnitCount GetFreeUnitCount() const;
    RegionUsageSnapshot GetRegionUsageSnapshot() const;
    size_t GetOwnedFreeRegionCount(uint16_t ownerShardId) const;
    size_t GetScavengedRegionCount(uint16_t ownerShardId) const;
    size_t GetReusedScavengedRegionCount() const
    {
        return reusedScavengedRegionCount_.load(std::memory_order_relaxed);
    }
    size_t GetHotFreeRegionCount(uint16_t ownerShardId) const;
    size_t GetBorrowedRegionCount(uint16_t receiverShardId) const;
    size_t GetOwnedSegmentCount(uint16_t ownerShardId) const;
    size_t GetOwnedTotalRegionCount(uint16_t ownerShardId) const;
    // Return bytes in free regions whose physical pages have NOT been returned
    // to the OS yet (hotFree = freeRegions - scavengedRegions).
    size_t GetHotFreeBytes() const;
    // Slow/reference scan used for threshold confirmation and aggregate
    // diagnostics. Normal Region acquisition must use GetHotFreeBytes().
    size_t GetHotFreeBytesReference() const;

    // Like ScavengeFreeRegions, but caps the work at maxBytes.
    // Returns actual bytes released (madvise'd).
    size_t ScavengeFreeRegionsUpTo(size_t maxBytes);

    uintptr_t GetCommittedEnd() const { return committedEnd_; }
    uintptr_t GetMaxHeapEnd() const { return maxHeapEnd_; }
    uintptr_t GetInactiveZone() const { return committedEnd_; }
    size_t GetCurrentCapacityBytes() const { return committedEnd_ - heapStart_; }
    size_t GetCommittedRegionCount() const { return committedRegionCount_; }
    uint64_t GetAvailabilityGeneration() const
    {
        return availabilityGeneration_.load(std::memory_order_acquire);
    }
    size_t GetMaxVirtualRegionCount() const { return maxVirtualRegionCount_; }
    uint16_t ResolveOwnerShardId(RegionIndex idx) const;
    size_t GetSegmentRegionCount() const { return segmentRegionCount_; }
    size_t GetCommittedSegmentCount() const
    {
        return segmentRegionCount_ == 0 ? 0 : (committedRegionCount_ + segmentRegionCount_ - 1) / segmentRegionCount_;
    }
    SegmentFreeSnapshot GetSegmentFreeSnapshot(size_t segmentIdx) const;
    size_t ReleaseCompletelyFreeSegment(size_t segmentIdx);

    UnitIndex RegionToUnitIdx(RegionIndex regionIdx) const;
    UnitCount RegionCountToUnitCount(RegionCount regionCount) const;

private:
    static constexpr RegionCount kSingleRegionStealBatch = 4;

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
        uint32_t maxFreeRun_ = 0;  // Opt3: max contiguous free run; skip segment when < num
        uint64_t hotWordMask_ = 0;
        std::vector<uint64_t> allocBits_;
        std::vector<uint64_t> scavengedBits_;
        std::vector<uint64_t> scavengingBits_;
        std::vector<ChunkSummary> chunkSummaries_;
    };

    struct PendingScavengeSpan {
        RegionIndex idx = 0;
        RegionCount num = 0;
        bool released = false;
    };

    struct FreeManagerShard {
        mutable std::mutex shardMutex_;
        uint16_t shardId_ = 0;
        size_t freeRegionCount_ = 0;
        size_t scavengedRegionCount_ = 0;
        size_t scavengingRegionCount_ = 0;
        RegionIndex takeCursor_ = 0;
        RegionIndex reclaimCursor_ = 0;
        std::atomic<uint16_t> nextStealDonorShard_{0};
        size_t borrowedRegionCount_ = 0;
        std::vector<size_t> ownedSegments_;
        std::vector<OwnedSegmentView> ownedViews_;
        std::deque<BorrowedSpan> borrowedSpans_;
        std::atomic<uint32_t> snapHotFreeRegions_{0};
        size_t publishedFreeRegionCount_ = 0;
        bool advertisedNonEmpty_ = false;
        bool fastTakeHintValid_ = false;
        size_t fastTakeViewIdx_ = 0;
        size_t fastTakeWordOffset_ = 0;
        size_t scavengeViewCursor_ = 0;
        size_t scavengeWordCursor_ = 0;
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

    static uint64_t ValidRegionMaskForWord(size_t wordIdx, size_t committedLocal);
    static uint64_t HotFreeMaskForWord(const OwnedSegmentView& view,
                                       size_t wordIdx,
                                       size_t committedLocal);
    static void RefreshHotWordLocked(OwnedSegmentView& view,
                                     size_t wordIdx,
                                     size_t committedLocal);
    static void RefreshHotWordsInRangeLocked(OwnedSegmentView& view,
                                             size_t localStart,
                                             RegionCount num,
                                             size_t committedLocal);
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
    uint16_t ResolveOwnerShardIdLocked(RegionIndex idx) const;
    RegionIndex FindNextFreeRegionLocked(RegionIndex start) const;
    RegionIndex FindNextOwnedFreeRegionLocked(const FreeManagerShard& shard, RegionIndex start) const;
    RegionIndex FindFirstFreeRegionInViewLocked(const OwnedSegmentView& view, RegionIndex start) const;
    bool PreciseFitFromLocked(RegionIndex start, RegionCount num) const;
    bool TryTakeBorrowedRegionLocked(FreeManagerShard& shard, RegionIndex& idx);
    bool FindFreeRunInRangeLocked(RegionIndex start, RegionIndex end, RegionCount num, RegionIndex& idx) const;
    bool FindFreeRunLocked(RegionIndex cursor, RegionCount num, RegionIndex& idx) const;
    bool FindOwnedFreeRunLocked(const FreeManagerShard& shard, RegionIndex cursor, RegionCount num, RegionIndex& idx) const;
    static RegionCount CountContiguousFreeInViewLocked(const OwnedSegmentView& view,
                                                       RegionIndex start,
                                                       RegionCount maxCount);
    // Opt1: word-level fit check within a view (no per-region view lookup)
    static bool PreciseFitInViewFromLocked(const OwnedSegmentView& view, size_t localStart, size_t num);
    // Opt2 helpers: word-level ctz scan within a view
    static size_t FindFirstFreeLocalLocked(const OwnedSegmentView& view, size_t localPos, size_t localEnd);
    static size_t FindFirstAllocLocalLocked(const OwnedSegmentView& view, size_t localPos, size_t localEnd);
    // Opt2: chunk-summary guided run search within a single view
    static bool FindFreeRunInViewLocked(const OwnedSegmentView& view, size_t localStart, size_t localEnd,
                                        size_t num, size_t& localIdx);
    void RefreshRegionSnapshotsLocked(FreeManagerShard& shard);
    size_t ClaimScavengeSpansLocked(FreeManagerShard& shard,
                                    size_t regionBudget,
                                    std::vector<PendingScavengeSpan>& spans);
    size_t ClaimScavengeWordMaskLocked(FreeManagerShard& shard,
                                       OwnedSegmentView& view,
                                       size_t wordIdx,
                                       uint64_t claimMask,
                                       std::vector<PendingScavengeSpan>& spans);
    size_t ClaimScavengeSpansInViewLocked(FreeManagerShard& shard,
                                          OwnedSegmentView& view,
                                          size_t committedLocal,
                                          size_t regionBudget,
                                          std::vector<PendingScavengeSpan>& spans);
    void AppendGroupedOwnerSpansLocked(std::vector<std::vector<RegionSpan>>& groupedSpans,
                                       RegionIndex idx,
                                       RegionCount num) const;
    void ReclaimRegionsReleasedImpl(const RegionSpan* spans, size_t count);
    void ApplyGroupedOwnerReclaim(std::vector<std::vector<RegionSpan>>& groupedSpans);
    size_t ScavengeFreeRegionsByRegionBudget(size_t regionBudget);
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
    bool GrowOneSegmentLocked(uint16_t ownerShardId, HeapRangeHandler& handler);
    bool GrowAndTakeRegionsLocked(RegionCount numRegions,
                                  uint16_t ownerShardId,
                                  RegionIndex& idx,
                                  HeapRangeHandler& handler);
    bool TryStealContiguousRegionRunLocked(uint16_t receiverShardId,
                                           RegionCount num,
                                           RegionIndex& idx,
                                           RegionSpan& reusedSpan);
    bool TryStealSingleRegionBatchLocked(uint16_t receiverShardId, RegionIndex& idx, RegionSpan& reusedSpan);
    bool FindNextNonEmptyDonorShard(uint16_t receiverShardId,
                                    uint32_t searchStart,
                                    uint16_t& donorShardId) const;
    bool FindNonEmptyShardInRange(uint16_t receiverShardId,
                                  uint32_t begin,
                                  uint32_t end,
                                  uint16_t& donorShardId) const;
    size_t CountScavengedInViewRangeLocked(const OwnedSegmentView& view, size_t localStart, RegionCount num) const;
    size_t CountFreeInViewRangeLocked(const OwnedSegmentView& view, size_t localStart, RegionCount num) const;
    void MarkAllocatedLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num, bool& reusedScavenged);
    void MarkFreeLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num);
    void MarkScavengingLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num);
    void CompleteScavengingLocked(FreeManagerShard& shard,
                                  RegionIndex idx,
                                  RegionCount num,
                                  bool released);
    void MarkUnavailableLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num);
    void UpdateChunkSummaryLocked(OwnedSegmentView& view, size_t chunkIdx);
    void UpdateChunkSummaryRangeLocked(OwnedSegmentView& view, size_t localStart, RegionCount num);
    size_t CountTrailingFreeRegionsLocked() const;
    bool ReleaseUnits(UnitIndex idx, UnitCount num) const;
    void ReuseUnits(UnitIndex idx, UnitCount num) const;
    void CheckAlignmentConfig() const;
    size_t GetRegionSizeBytes() const;
    RegionIndex AddrToRegionIdx(uintptr_t addr) const;
    uintptr_t RegionIdxToAddr(RegionIndex regionIdx) const;
    uint32_t ComputeMaxFreeRunInViewLocked(const OwnedSegmentView& view) const;

    mutable std::mutex pageHeapMutex_;
    mutable std::mutex growMutex_;
    size_t unitSize_ = 0;
    size_t unitsPerRegion_ = 0;
    uintptr_t heapStart_ = 0;
    uintptr_t maxHeapEnd_ = 0;
    uintptr_t committedEnd_ = 0;
    size_t committedRegionCount_ = 0;
    size_t maxVirtualRegionCount_ = 0;
    size_t segmentSize_ = 0;
    uint32_t shardCount_ = 1;
    size_t segmentRegionCount_ = 0;
    std::deque<FreeManagerShard> shards_;
    std::unique_ptr<std::atomic<uint64_t>[]> nonEmptyShardWords_;
    size_t nonEmptyShardWordCount_ = 0;
    std::atomic<size_t> totalHotFreeRegions_ {0};
    std::atomic<size_t> reusedScavengedRegionCount_ {0};
    std::atomic<uint64_t> availabilityGeneration_ {0};
    std::vector<uint16_t> segmentOwnerShard_;
    uint16_t nextScavengeShard_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_FREE_MANAGER_H
