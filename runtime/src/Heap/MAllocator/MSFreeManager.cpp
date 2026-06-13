// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSFreeManager.h"

#include <algorithm>
#include <cstring>

#ifndef _WIN64
#include <sys/mman.h>
#else
#include <windows.h>
#endif

#include "Base/Log.h"
#include "Base/MemUtils.h"

namespace MapleRuntime {

FreeManager::OwnedSegmentView FreeManager::BuildOwnedSegmentView(size_t segmentIdx, RegionCount regionCount) const
{
    OwnedSegmentView view {};
    view.segmentIdx_ = segmentIdx;
    view.regionStart_ = static_cast<RegionIndex>(segmentIdx * segmentRegionCount_);
    view.regionEnd_ = static_cast<RegionIndex>(view.regionStart_ + regionCount);
    view.bitmapWordCount_ = RegionCountToChunkCount(regionCount);
    view.summaryChunkCount_ = RegionCountToChunkCount(regionCount);
    view.allocBits_.assign(view.bitmapWordCount_, 0);
    view.scavengedBits_.assign(view.bitmapWordCount_, 0);
    view.chunkSummaries_.assign(view.summaryChunkCount_, {});
    view.maxFreeRun_ = regionCount;  // freshly grown: entirely free
    return view;
}

void FreeManager::AttachOwnedSegmentLocked(FreeManagerShard& shard, size_t segmentIdx, RegionCount regionCount)
{
    shard.ownedSegments_.push_back(segmentIdx);
    OwnedSegmentView view = BuildOwnedSegmentView(segmentIdx, regionCount);
    shard.bitmapWordCountInUse_ += view.bitmapWordCount_;
    shard.summaryChunkCountInUse_ += view.summaryChunkCount_;
    shard.freeRegionCount_ += regionCount;
    shard.ownedViews_.push_back(view);
    shard.fastTakeHintValid_ = false;
}

void FreeManager::DetachOwnedSegmentLocked(FreeManagerShard& shard)
{
    if (shard.ownedSegments_.empty() || shard.ownedViews_.empty()) {
        return;
    }

    const OwnedSegmentView& view = shard.ownedViews_.back();
    shard.bitmapWordCountInUse_ -= std::min(shard.bitmapWordCountInUse_, view.bitmapWordCount_);
    shard.summaryChunkCountInUse_ -= std::min(shard.summaryChunkCountInUse_, view.summaryChunkCount_);
    shard.ownedSegments_.pop_back();
    shard.ownedViews_.pop_back();
    shard.fastTakeHintValid_ = false;
}

FreeManager::FreeManagerShard& FreeManager::GetShard(uint16_t ownerShardId)
{
    CHECK_DETAIL(!shards_.empty(), "[MS-GC] shard state is not initialized");
    ownerShardId = static_cast<uint16_t>(static_cast<uint32_t>(ownerShardId) % shardCount_);
    return shards_[ownerShardId];
}

const FreeManager::FreeManagerShard& FreeManager::GetShard(uint16_t ownerShardId) const
{
    CHECK_DETAIL(!shards_.empty(), "[MS-GC] shard state is not initialized");
    ownerShardId = static_cast<uint16_t>(static_cast<uint32_t>(ownerShardId) % shardCount_);
    return shards_[ownerShardId];
}

FreeManager::OwnedSegmentView* FreeManager::FindOwnedSegmentViewBySegmentIdxLocked(FreeManagerShard& shard, size_t segmentIdx)
{
    for (OwnedSegmentView& view : shard.ownedViews_) {
        if (view.segmentIdx_ == segmentIdx) {
            return &view;
        }
    }
    return nullptr;
}

const FreeManager::OwnedSegmentView* FreeManager::FindOwnedSegmentViewBySegmentIdxLocked(
    const FreeManagerShard& shard, size_t segmentIdx) const
{
    for (const OwnedSegmentView& view : shard.ownedViews_) {
        if (view.segmentIdx_ == segmentIdx) {
            return &view;
        }
    }
    return nullptr;
}

FreeManager::OwnedSegmentView* FreeManager::FindOwnedSegmentViewByRegionLocked(FreeManagerShard& shard, RegionIndex idx)
{
    for (OwnedSegmentView& view : shard.ownedViews_) {
        if (idx >= view.regionStart_ && idx < view.regionEnd_) {
            return &view;
        }
    }
    return nullptr;
}

const FreeManager::OwnedSegmentView* FreeManager::FindOwnedSegmentViewByRegionLocked(
    const FreeManagerShard& shard, RegionIndex idx) const
{
    for (const OwnedSegmentView& view : shard.ownedViews_) {
        if (idx >= view.regionStart_ && idx < view.regionEnd_) {
            return &view;
        }
    }
    return nullptr;
}

void FreeManager::Initialize(UnitCount maxUnitCount, size_t unitSize, size_t unitsPerRegion,
                             RegionCount committedRegionCount, RegionCount maxVirtualRegionCount,
                             uintptr_t heapStart, uintptr_t committedEnd, uintptr_t maxHeapEnd,
                             size_t segmentSize, uint32_t shardCount)
{
    CHECK_DETAIL(unitSize != 0, "[MS-GC] FreeManager unitSize must not be zero");
    CHECK_DETAIL(unitsPerRegion != 0, "[MS-GC] FreeManager unitsPerRegion must not be zero");
    CHECK_DETAIL(heapStart <= committedEnd, "[MS-GC] FreeManager committed end is below heap start");
    CHECK_DETAIL(committedEnd <= maxHeapEnd, "[MS-GC] FreeManager max heap end is below committed end");

    unitSize_ = unitSize;
    unitsPerRegion_ = unitsPerRegion;
    heapStart_ = heapStart;
    committedEnd_ = committedEnd;
    maxHeapEnd_ = maxHeapEnd;
    committedRegionCount_ = static_cast<size_t>(committedRegionCount);
    committedUnitCount_ = static_cast<size_t>(RegionCountToUnitCount(committedRegionCount));
    maxVirtualRegionCount_ = static_cast<size_t>(maxVirtualRegionCount);
    maxVirtualUnitCount_ = static_cast<size_t>(maxUnitCount);
    segmentSize_ = segmentSize;
    shardCount_ = NormalizeMSShardCount(shardCount);
    segmentRegionCount_ = segmentSize_ / GetRegionSizeBytes();
    CHECK_DETAIL(segmentRegionCount_ != 0,
                 "[MS-GC] FreeManager segmentRegionCount must not be zero, segmentSize=%zu regionSize=%zu",
                 segmentSize_, GetRegionSizeBytes());
    heapGrowCount_ = 0;
    shards_.clear();
    for (uint32_t shardIdx = 0; shardIdx < shardCount_; ++shardIdx) {
        shards_.emplace_back();
    }
    size_t segmentCount = (maxVirtualRegionCount_ + segmentRegionCount_ - 1) / segmentRegionCount_;
    segmentOwnerShard_.assign(segmentCount, 0);
    nextScavengeShard_ = 0;
    size_t committedSegmentCount = (committedRegionCount_ + segmentRegionCount_ - 1) / segmentRegionCount_;
    for (size_t segmentIdx = 0; segmentIdx < committedSegmentCount; ++segmentIdx) {
        uint16_t ownerShardId = static_cast<uint16_t>(segmentIdx % shardCount_);
        segmentOwnerShard_[segmentIdx] = ownerShardId;
        FreeManagerShard& shardState = GetShard(ownerShardId);
        RegionIndex segmentStart = static_cast<RegionIndex>(segmentIdx * segmentRegionCount_);
        RegionCount ownedRegionCount = static_cast<RegionCount>(
            std::min(committedRegionCount_ - static_cast<size_t>(segmentStart), segmentRegionCount_));
        AttachOwnedSegmentLocked(shardState, segmentIdx, ownedRegionCount);
    }
    for (uint32_t shardIdx = 0; shardIdx < shardCount_; ++shardIdx) {
        FreeManagerShard& shardState = GetShard(static_cast<uint16_t>(shardIdx));
        shardState.nextStealDonorShard_.store(
            static_cast<uint16_t>((shardIdx + 1) % shardCount_), std::memory_order_relaxed);
        if (!shardState.ownedViews_.empty()) {
            shardState.takeCursor_ = shardState.ownedViews_.front().regionStart_;
        } else {
            shardState.takeCursor_ = static_cast<RegionIndex>(committedRegionCount_);
        }
        shardState.reclaimCursor_ = static_cast<RegionIndex>(committedRegionCount_);
        shardState.fastTakeHintValid_ = false;
        shardState.scavengedRegionCount_ = 0;
        for (OwnedSegmentView& view : shardState.ownedViews_) {
            for (size_t chunkIdx = 0; chunkIdx < view.chunkSummaries_.size(); ++chunkIdx) {
                UpdateChunkSummaryLocked(view, chunkIdx);
            }
        }
    }
    for (uint32_t shardIdx = 0; shardIdx < shardCount_; ++shardIdx) {
        FreeManagerShard& shardState = GetShard(static_cast<uint16_t>(shardIdx));
        RefreshShardMaxOwnedFreeRunLocked(shardState);
    }
}

FreeManager::TryTakeResult FreeManager::TryTakeRegions(RegionCount num, uint16_t homeShardId, RegionIndex& idx)
{
    if (num == 0) {
        return TryTakeResult::EMPTY;
    }

    bool reusedScavenged = false;
    RegionSpan reusedSpan {};
    bool found = false;
    {
        FreeManagerShard& shardState = GetShard(homeShardId);
        std::lock_guard<std::mutex> shardLock(shardState.shardMutex_);
        if (num == 1 && TryTakeBorrowedRegionLocked(shardState, idx)) {
            return TryTakeResult::SUCCESS;
        }
        found = TryTakeLocalRegionsLocked(shardState, num, idx, reusedScavenged);
    }

    if (!found && num == 1) {
        found = TryStealSingleRegionBatchLocked(homeShardId, idx, reusedSpan);
    }

    if (!found) {
        return TryTakeResult::EMPTY;
    }

    if (reusedScavenged) {
        ReuseUnits(RegionToUnitIdx(idx), RegionCountToUnitCount(num));
    }
    if (reusedSpan.num != 0) {
        ReuseUnits(RegionToUnitIdx(reusedSpan.idx), RegionCountToUnitCount(reusedSpan.num));
    }
    return TryTakeResult::SUCCESS;
}

bool FreeManager::TryTakeLocalRegionsLocked(FreeManagerShard& shard,
                                            RegionCount num,
                                            RegionIndex& idx,
                                            bool& reusedScavenged)
{
    if (num == 1 && TryTakeOneRegionFastLocked(shard, idx, reusedScavenged)) {
        return true;
    }

    bool fromTakeSide = FindOwnedFreeRunLocked(shard, shard.takeCursor_, num, idx);
    bool fromReclaimSide = false;
    if (!fromTakeSide && shard.reclaimCursor_ < shard.takeCursor_) {
        fromReclaimSide = FindOwnedFreeRunLocked(shard, shard.reclaimCursor_, num, idx);
    }
    if (!fromTakeSide && !fromReclaimSide) {
        return false;
    }

    MarkAllocatedLocked(shard, idx, num, reusedScavenged);
    if (fromTakeSide && idx == shard.takeCursor_) {
        shard.takeCursor_ = FindNextOwnedFreeRegionLocked(shard, idx + num);
    }
    if (fromReclaimSide && idx == shard.reclaimCursor_) {
        shard.reclaimCursor_ = FindNextOwnedFreeRegionLocked(shard, idx + num);
    }
    return true;
}

bool FreeManager::TryIncreaseInactiveZone(RegionCount num, RegionIndex& idx)
{
    (void)num;
    (void)idx;
    return false;
}

void FreeManager::ReclaimRegionsReleased(RegionIndex idx, RegionCount num)
{
    if (num == 0) {
        return;
    }

    std::vector<std::vector<RegionSpan>> groupedSpans(shardCount_);
    {
        std::lock_guard<std::mutex> pageLock(pageHeapMutex_);
        AppendGroupedOwnerSpansLocked(groupedSpans, idx, num);
    }
    ApplyGroupedOwnerReclaim(groupedSpans);
}

void FreeManager::ReclaimRegionsReleasedBatch(const std::vector<RegionSpan>& spans)
{
    if (spans.empty()) {
        return;
    }

    std::vector<std::vector<RegionSpan>> groupedSpans(shardCount_);
    {
        std::lock_guard<std::mutex> pageLock(pageHeapMutex_);
        for (const RegionSpan& span : spans) {
            AppendGroupedOwnerSpansLocked(groupedSpans, span.idx, span.num);
        }
    }
    ApplyGroupedOwnerReclaim(groupedSpans);
}

// GrowOneSegmentLocked — inner grow logic; growMutex_ must be held by caller.
bool FreeManager::GrowOneSegmentLocked(uint16_t ownerShardId, HeapRangeHandler& handler)
{
    if (committedEnd_ >= maxHeapEnd_) {
        return false;
    }

    uintptr_t oldCommittedEnd = committedEnd_;
    size_t growBytes = segmentSize_;
    if (oldCommittedEnd + growBytes > maxHeapEnd_) {
        growBytes = maxHeapEnd_ - oldCommittedEnd;
    }

    RegionCount growRegions = static_cast<RegionCount>(growBytes / GetRegionSizeBytes());
    UnitCount growUnits = static_cast<UnitCount>(growBytes / unitSize_);
    RegionIndex oldRegionCount = static_cast<RegionIndex>(committedRegionCount_);
    size_t newCommittedRegionCount = committedRegionCount_ + static_cast<size_t>(growRegions);
    size_t oldCommittedRegionCount = committedRegionCount_;
    ownerShardId = static_cast<uint16_t>(static_cast<uint32_t>(ownerShardId) % shardCount_);

    handler.InitializeFreshRegions(oldRegionCount, growRegions);

    FreeManagerShard& shardState = GetShard(ownerShardId);
    std::lock_guard<std::mutex> shardLock(shardState.shardMutex_);
    {
        std::lock_guard<std::mutex> pageLock(pageHeapMutex_);
        committedRegionCount_ = newCommittedRegionCount;
        committedUnitCount_ += static_cast<size_t>(growUnits);
        committedEnd_ = oldCommittedEnd + growBytes;
        size_t growStartSegmentIdx = static_cast<size_t>(oldRegionCount) / segmentRegionCount_;
        size_t growEndRegion = oldCommittedRegionCount + static_cast<size_t>(growRegions);
        size_t growEndSegmentIdx = (growEndRegion + segmentRegionCount_ - 1) / segmentRegionCount_;
        for (size_t segmentIdx = growStartSegmentIdx; segmentIdx < growEndSegmentIdx; ++segmentIdx) {
            size_t segmentStartRegion = segmentIdx * segmentRegionCount_;
            if (segmentStartRegion < oldCommittedRegionCount) {
                continue;
            }
            if (segmentIdx < segmentOwnerShard_.size()) {
                segmentOwnerShard_[segmentIdx] = ownerShardId;
                RegionCount ownedRegionCount = static_cast<RegionCount>(
                    std::min(newCommittedRegionCount - segmentStartRegion, segmentRegionCount_));
                AttachOwnedSegmentLocked(shardState, segmentIdx, ownedRegionCount);
                OwnedSegmentView* view = FindOwnedSegmentViewBySegmentIdxLocked(shardState, segmentIdx);
                CHECK_DETAIL(view != nullptr, "[MS-GC] grown segment view missing: segment=%zu", segmentIdx);
                for (size_t chunkIdx = 0; chunkIdx < view->chunkSummaries_.size(); ++chunkIdx) {
                    UpdateChunkSummaryLocked(*view, chunkIdx);
                }
            }
        }
        if (shardState.ownedViews_.size() == 1) {
            shardState.takeCursor_ = oldRegionCount;
        } else {
            shardState.takeCursor_ = std::min(shardState.takeCursor_, oldRegionCount);
        }
        shardState.reclaimCursor_ = std::min(shardState.reclaimCursor_,
                                             static_cast<RegionIndex>(committedRegionCount_));
        shardState.fastTakeHintValid_ = false;
    }

    handler.OnCommittedHeapExtended(committedEnd_);
    heapGrowCount_++;
    return true;
}

bool FreeManager::GrowOneSegment(uint16_t ownerShardId, HeapRangeHandler& handler)
{
    std::lock_guard<std::mutex> growLock(growMutex_);
    return GrowOneSegmentLocked(ownerShardId, handler);
}

// GrowForRegions — grow enough segments to hold numRegions contiguous regions,
// all assigned to ownerShardId, under a single growMutex_ acquisition so that
// the resulting segments are physically adjacent in the heap.
bool FreeManager::GrowForRegions(RegionCount numRegions, uint16_t ownerShardId, HeapRangeHandler& handler)
{
    size_t numSegments = (segmentRegionCount_ > 0)
        ? (static_cast<size_t>(numRegions) + segmentRegionCount_ - 1) / segmentRegionCount_
        : 1;
    std::lock_guard<std::mutex> growLock(growMutex_);
    bool grew = false;
    for (size_t i = 0; i < numSegments; ++i) {
        if (!GrowOneSegmentLocked(ownerShardId, handler)) {
            break;
        }
        grew = true;
    }
    return grew;
}

void FreeManager::ShrinkHeap()
{
    std::lock_guard<std::mutex> growLock(growMutex_);
    std::lock_guard<std::mutex> pageLock(pageHeapMutex_);

    size_t trailingFree = CountTrailingFreeRegionsLocked();
    if (trailingFree == 0) {
        return;
    }

    size_t trailingBytes = trailingFree * GetRegionSizeBytes();
    size_t shrinkSegments = trailingBytes / segmentSize_;
    if (shrinkSegments == 0) {
        return;
    }

    size_t shrinkBytes = shrinkSegments * segmentSize_;
    size_t shrinkRegions = shrinkBytes / GetRegionSizeBytes();
    size_t minCommittedRegions = segmentSize_ / GetRegionSizeBytes();
    if (committedRegionCount_ <= minCommittedRegions) {
        return;
    }
    if (shrinkRegions > committedRegionCount_ - minCommittedRegions) {
        shrinkRegions = committedRegionCount_ - minCommittedRegions;
        shrinkBytes = shrinkRegions * GetRegionSizeBytes();
        shrinkSegments = shrinkRegions / segmentRegionCount_;
    }
    if (shrinkRegions == 0) {
        return;
    }

    RegionIndex shrinkStart = static_cast<RegionIndex>(committedRegionCount_ - shrinkRegions);
    size_t shrinkStartSegmentIdx = static_cast<size_t>(shrinkStart) / segmentRegionCount_;

    ReleaseUnits(RegionToUnitIdx(shrinkStart), RegionCountToUnitCount(static_cast<RegionCount>(shrinkRegions)));

    // Process each segment from highest index to lowest so that when each segment is processed,
    // it is still at back() of its owning shard's ownedSegments_ (higher segments have already
    // been detached).  This allows DetachOwnedSegmentLocked to remove the correct entry even
    // when the shrink range spans segments owned by different shards.
    for (size_t segIdx = shrinkStartSegmentIdx + shrinkSegments; segIdx-- > shrinkStartSegmentIdx; ) {
        uint16_t shardId = (segIdx < segmentOwnerShard_.size()) ? segmentOwnerShard_[segIdx] : 0;
        FreeManagerShard& shard = GetShard(shardId);
        std::lock_guard<std::mutex> shardLock(shard.shardMutex_);
        RegionIndex segStart = static_cast<RegionIndex>(segIdx * segmentRegionCount_);
        MarkUnavailableLocked(shard, segStart, static_cast<RegionCount>(segmentRegionCount_));
        DetachOwnedSegmentLocked(shard);
    }

    committedRegionCount_ -= shrinkRegions;
    committedUnitCount_ = committedRegionCount_ * unitsPerRegion_;
    committedEnd_ -= shrinkBytes;

    // Fix up cursors for every shard that lost at least one segment.
    std::vector<bool> shardVisited(shardCount_, false);
    for (size_t segIdx = shrinkStartSegmentIdx + shrinkSegments; segIdx-- > shrinkStartSegmentIdx; ) {
        uint16_t shardId = (segIdx < segmentOwnerShard_.size()) ? segmentOwnerShard_[segIdx] : 0;
        if (shardId < shardCount_ && shardVisited[shardId]) {
            continue;
        }
        if (shardId < shardCount_) {
            shardVisited[shardId] = true;
        }
        FreeManagerShard& shard = GetShard(shardId);
        std::lock_guard<std::mutex> shardLock(shard.shardMutex_);
        if (shard.ownedSegments_.empty()) {
            shard.takeCursor_ = static_cast<RegionIndex>(committedRegionCount_);
            shard.reclaimCursor_ = static_cast<RegionIndex>(committedRegionCount_);
        } else {
            if (shard.takeCursor_ >= committedRegionCount_) {
                shard.takeCursor_ = FindNextOwnedFreeRegionLocked(shard, 0);
            }
            shard.reclaimCursor_ = std::min(shard.reclaimCursor_,
                                             static_cast<RegionIndex>(committedRegionCount_));
        }
        RefreshShardMaxOwnedFreeRunLocked(shard);
    }
}

size_t FreeManager::ScavengeFreeRegions(bool releaseAll)
{
    if (committedRegionCount_ == 0) {
        return 0;
    }

    size_t regionBudget = releaseAll ? committedRegionCount_ : std::max<size_t>(1, segmentSize_ / GetRegionSizeBytes());
    size_t scavengedRegions = 0;
    uint16_t startShardId = nextScavengeShard_;
    for (uint32_t scannedShardCount = 0;
         scannedShardCount < shardCount_ && scavengedRegions < regionBudget;
         ++scannedShardCount) {
        uint16_t ownerShardId = static_cast<uint16_t>((startShardId + scannedShardCount) % shardCount_);
        FreeManagerShard& shardState = GetShard(ownerShardId);
        std::unique_lock<std::mutex> shardLock(shardState.shardMutex_, std::try_to_lock);
        if (!shardLock.owns_lock()) {
            continue;
        }
        scavengedRegions += ScavengeOwnedFreeRegionsLocked(shardState, ownerShardId, regionBudget - scavengedRegions);
    }
    nextScavengeShard_ = static_cast<uint16_t>((static_cast<uint32_t>(startShardId) + 1) % shardCount_);

    return scavengedRegions * GetRegionSizeBytes();
}

FreeManager::UnitCount FreeManager::GetReleasedUnitCount() const
{
    size_t totalScavengedRegions = 0;
    for (uint32_t shardIdx = 0; shardIdx < shardCount_; ++shardIdx) {
        const FreeManagerShard& shardState = GetShard(static_cast<uint16_t>(shardIdx));
        std::lock_guard<std::mutex> lg(shardState.shardMutex_);
        totalScavengedRegions += shardState.scavengedRegionCount_;
    }
    return static_cast<UnitCount>(totalScavengedRegions * unitsPerRegion_);
}

FreeManager::UnitCount FreeManager::GetFreeUnitCount() const
{
    size_t totalFreeRegions = 0;
    for (uint32_t shardIdx = 0; shardIdx < shardCount_; ++shardIdx) {
        const FreeManagerShard& shardState = GetShard(static_cast<uint16_t>(shardIdx));
        std::lock_guard<std::mutex> lg(shardState.shardMutex_);
        totalFreeRegions += shardState.freeRegionCount_;
    }
    return static_cast<UnitCount>(totalFreeRegions * unitsPerRegion_);
}

size_t FreeManager::GetReleasedBytes() const
{
    return static_cast<size_t>(GetReleasedUnitCount()) * unitSize_;
}

size_t FreeManager::GetOwnedFreeRegionCount(uint16_t ownerShardId) const
{
    const FreeManagerShard& shardState = GetShard(ownerShardId);
    std::lock_guard<std::mutex> lg(shardState.shardMutex_);
    return shardState.freeRegionCount_;
}

size_t FreeManager::GetScavengedRegionCount(uint16_t ownerShardId) const
{
    const FreeManagerShard& shardState = GetShard(ownerShardId);
    std::lock_guard<std::mutex> lg(shardState.shardMutex_);
    return shardState.scavengedRegionCount_;
}

size_t FreeManager::GetHotFreeRegionCount(uint16_t ownerShardId) const
{
    const FreeManagerShard& shardState = GetShard(ownerShardId);
    return shardState.snapHotFreeRegions_.load(std::memory_order_relaxed);
}

size_t FreeManager::GetBorrowedRegionCount(uint16_t receiverShardId) const
{
    const FreeManagerShard& shardState = GetShard(receiverShardId);
    std::lock_guard<std::mutex> lg(shardState.shardMutex_);
    return shardState.borrowedNum_;
}

size_t FreeManager::GetOwnedSegmentCount(uint16_t ownerShardId) const
{
    const FreeManagerShard& shardState = GetShard(ownerShardId);
    std::lock_guard<std::mutex> lg(shardState.shardMutex_);
    return shardState.ownedSegments_.size();
}

size_t FreeManager::GetOwnedTotalRegionCount(uint16_t ownerShardId) const
{
    const FreeManagerShard& shardState = GetShard(ownerShardId);
    std::lock_guard<std::mutex> lg(shardState.shardMutex_);
    size_t total = 0;
    for (const OwnedSegmentView& view : shardState.ownedViews_) {
        total += static_cast<size_t>(view.regionEnd_ - view.regionStart_);
    }
    return total;
}

FreeManager::RegionIndex FreeManager::GetShardTakeCursor(uint16_t ownerShardId) const
{
    const FreeManagerShard& shardState = GetShard(ownerShardId);
    std::lock_guard<std::mutex> lg(shardState.shardMutex_);
    return shardState.takeCursor_;
}

FreeManager::RegionIndex FreeManager::GetShardReclaimCursor(uint16_t ownerShardId) const
{
    const FreeManagerShard& shardState = GetShard(ownerShardId);
    std::lock_guard<std::mutex> lg(shardState.shardMutex_);
    return shardState.reclaimCursor_;
}

size_t FreeManager::GetHotFreeBytes() const
{
    size_t totalHotFreeRegions = 0;
    for (uint32_t shardIdx = 0; shardIdx < shardCount_; ++shardIdx) {
        totalHotFreeRegions += GetHotFreeRegionCount(static_cast<uint16_t>(shardIdx));
    }
    return totalHotFreeRegions * GetRegionSizeBytes();
}

size_t FreeManager::ScavengeFreeRegionsUpTo(size_t maxBytes)
{
    if (maxBytes == 0) {
        return 0;
    }
    if (committedRegionCount_ == 0) {
        return 0;
    }
    size_t regionSize = GetRegionSizeBytes();
    size_t regionBudget = (maxBytes + regionSize - 1) / regionSize;
    if (regionBudget == 0) {
        return 0;
    }

    size_t scavengedRegions = 0;
    uint16_t startShardId = nextScavengeShard_;
    for (uint32_t scannedShardCount = 0;
         scannedShardCount < shardCount_ && scavengedRegions < regionBudget;
         ++scannedShardCount) {
        uint16_t ownerShardId = static_cast<uint16_t>((startShardId + scannedShardCount) % shardCount_);
        FreeManagerShard& shardState = GetShard(ownerShardId);
        std::unique_lock<std::mutex> shardLock(shardState.shardMutex_, std::try_to_lock);
        if (!shardLock.owns_lock()) {
            continue;
        }
        scavengedRegions += ScavengeOwnedFreeRegionsLocked(shardState, ownerShardId, regionBudget - scavengedRegions);
    }
    nextScavengeShard_ = static_cast<uint16_t>((static_cast<uint32_t>(startShardId) + 1) % shardCount_);

    return scavengedRegions * regionSize;
}

uint32_t FreeManager::GetMaxBlockRegions() const
{
    uint32_t maxRun = 0;
    for (uint32_t shardIdx = 0; shardIdx < shardCount_; ++shardIdx) {
        const FreeManagerShard& shardState = GetShard(static_cast<uint16_t>(shardIdx));
        std::lock_guard<std::mutex> lg(shardState.shardMutex_);
        maxRun = std::max(maxRun, shardState.maxOwnedFreeRun_);
    }
    return maxRun;
}

size_t FreeManager::GetMetadataBytesInUse() const
{
    size_t metadataBytes = 0;
    for (uint32_t shardIdx = 0; shardIdx < shardCount_; ++shardIdx) {
        const FreeManagerShard& shardState = GetShard(static_cast<uint16_t>(shardIdx));
        std::lock_guard<std::mutex> lg(shardState.shardMutex_);
        metadataBytes += shardState.bitmapWordCountInUse_ * sizeof(uint64_t) * 2;
        metadataBytes += shardState.summaryChunkCountInUse_ * sizeof(ChunkSummary);
    }
    return metadataBytes;
}

FreeManager::FinalStats FreeManager::GetFinalStats() const
{
    FinalStats stats;
    stats.committedRegions = committedRegionCount_;
    stats.committedBytes = committedRegionCount_ * GetRegionSizeBytes();
    for (uint32_t shardIdx = 0; shardIdx < shardCount_; ++shardIdx) {
        const FreeManagerShard& shardState = GetShard(static_cast<uint16_t>(shardIdx));
        std::lock_guard<std::mutex> lg(shardState.shardMutex_);
        stats.freeRegions += shardState.freeRegionCount_;
        stats.scavengedRegions += shardState.scavengedRegionCount_;
        stats.maxFreeRunRegions = std::max(stats.maxFreeRunRegions, shardState.maxOwnedFreeRun_);
    }
    stats.freeBytes = stats.freeRegions * GetRegionSizeBytes();
    stats.scavengedBytes = stats.scavengedRegions * GetRegionSizeBytes();
    stats.hotFreeRegions = (stats.freeRegions >= stats.scavengedRegions) ?
        (stats.freeRegions - stats.scavengedRegions) : 0;
    stats.hotFreeBytes = stats.hotFreeRegions * GetRegionSizeBytes();
    stats.usedRegions = (committedRegionCount_ >= stats.freeRegions) ?
        (committedRegionCount_ - stats.freeRegions) : 0;
    stats.usedBytes = stats.usedRegions * GetRegionSizeBytes();
    stats.metadataBytes = GetMetadataBytesInUse();
    stats.heapGrowCount = heapGrowCount_;
    return stats;
}

void FreeManager::CollectRegionSpans(std::vector<RegionSpan>& spans) const
{
    std::lock_guard<std::mutex> lg(pageHeapMutex_);
    spans.clear();

    RegionIndex idx = 0;
    while (idx < committedRegionCount_) {
        idx = FindNextFreeRegionLocked(idx);
        if (idx >= committedRegionCount_) {
            break;
        }
        RegionIndex start = idx;
        while (idx < committedRegionCount_ && IsRegionFreeLocked(idx)) {
            ++idx;
        }
        spans.push_back({ start, static_cast<RegionCount>(idx - start) });
    }
}

bool FreeManager::IsRegionFreeLocked(RegionIndex idx) const
{
    if (idx >= committedRegionCount_) {
        return false;
    }
    uint16_t ownerShardId = ResolveOwnerShardIdLocked(idx);
    const FreeManagerShard& shardState = GetShard(ownerShardId);
    const OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shardState, idx);
    if (view == nullptr) {
        return false;
    }
    size_t localIdx = static_cast<size_t>(idx - view->regionStart_);
    return !TestBit(view->allocBits_.data(), localIdx);
}

bool FreeManager::IsRegionScavengedLocked(RegionIndex idx) const
{
    if (idx >= committedRegionCount_) {
        return false;
    }
    uint16_t ownerShardId = ResolveOwnerShardIdLocked(idx);
    const FreeManagerShard& shardState = GetShard(ownerShardId);
    const OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shardState, idx);
    if (view == nullptr) {
        return false;
    }
    size_t localIdx = static_cast<size_t>(idx - view->regionStart_);
    return TestBit(view->scavengedBits_.data(), localIdx);
}

uint16_t FreeManager::ResolveOwnerShardIdLocked(RegionIndex idx) const
{
    if (segmentRegionCount_ == 0 || segmentOwnerShard_.empty()) {
        return 0;
    }
    size_t segmentIdx = static_cast<size_t>(idx) / segmentRegionCount_;
    if (segmentIdx >= segmentOwnerShard_.size()) {
        return 0;
    }
    return segmentOwnerShard_[segmentIdx];
}

FreeManager::RegionIndex FreeManager::FindNextFreeRegionLocked(RegionIndex start) const
{
    if (start >= committedRegionCount_) {
        return static_cast<RegionIndex>(committedRegionCount_);
    }

    size_t startSegmentIdx = static_cast<size_t>(start) / segmentRegionCount_;
    size_t committedSegmentCount = (committedRegionCount_ + segmentRegionCount_ - 1) / segmentRegionCount_;
    for (size_t segmentIdx = startSegmentIdx; segmentIdx < committedSegmentCount; ++segmentIdx) {
        uint16_t ownerShardId = segmentOwnerShard_[segmentIdx];
        const FreeManagerShard& shardState = GetShard(ownerShardId);
        const OwnedSegmentView* view = FindOwnedSegmentViewBySegmentIdxLocked(shardState, segmentIdx);
        if (view == nullptr) {
            continue;
        }
        RegionIndex probeStart = std::max(start, view->regionStart_);
        RegionIndex candidate = FindFirstFreeRegionInViewLocked(*view, probeStart);
        if (candidate < std::min(view->regionEnd_, static_cast<RegionIndex>(committedRegionCount_))) {
            return candidate;
        }
    }
    return static_cast<RegionIndex>(committedRegionCount_);
}

FreeManager::RegionIndex FreeManager::FindNextOwnedFreeRegionLocked(const FreeManagerShard& shard, RegionIndex start) const
{
    if (shard.ownedViews_.empty()) {
        return static_cast<RegionIndex>(committedRegionCount_);
    }

    RegionIndex best = static_cast<RegionIndex>(committedRegionCount_);
    for (const OwnedSegmentView& view : shard.ownedViews_) {
        RegionIndex segmentStart = view.regionStart_;
        RegionIndex segmentEnd = std::min(view.regionEnd_, static_cast<RegionIndex>(committedRegionCount_));
        if (segmentStart >= committedRegionCount_ || segmentEnd <= start) {
            continue;
        }
        RegionIndex probe = std::max(start, segmentStart);
        RegionIndex candidate = FindFirstFreeRegionInViewLocked(view, probe);
        if (candidate < segmentEnd) {
            best = std::min(best, candidate);
        }
    }
    return best;
}

FreeManager::RegionIndex FreeManager::FindFirstFreeRegionInViewLocked(const OwnedSegmentView& view, RegionIndex start) const
{
    RegionIndex viewStart = view.regionStart_;
    RegionIndex viewEnd = std::min(view.regionEnd_, static_cast<RegionIndex>(committedRegionCount_));
    if (viewStart >= viewEnd) {
        return static_cast<RegionIndex>(committedRegionCount_);
    }

    RegionIndex cursor = std::max(start, viewStart);
    if (cursor >= viewEnd) {
        return static_cast<RegionIndex>(committedRegionCount_);
    }

    size_t localStart = static_cast<size_t>(cursor - view.regionStart_);
    size_t wordIdx = localStart / kChunkBits;
    size_t wordLimit = view.bitmapWordCount_;
    for (; wordIdx < wordLimit; ++wordIdx) {
        size_t wordRegionStart = wordIdx * kChunkBits;
        RegionIndex wordStart = static_cast<RegionIndex>(view.regionStart_ + wordRegionStart);
        RegionIndex wordEnd = static_cast<RegionIndex>(
            std::min<size_t>(viewEnd, wordStart + kChunkBits));
        if (wordEnd <= viewStart) {
            continue;
        }
        if (wordStart >= viewEnd) {
            break;
        }

        uint64_t freeMask = ~view.allocBits_[wordIdx];
        RegionIndex scanStart = std::max(cursor, std::max(wordStart, viewStart));
        if (scanStart > wordStart) {
            unsigned skipBits = static_cast<unsigned>(scanStart - wordStart);
            if (skipBits >= kChunkBits) {
                continue;
            }
            freeMask &= ~((1ULL << skipBits) - 1);
        }

        if (wordEnd > viewEnd) {
            size_t keepBits = static_cast<size_t>(viewEnd - wordStart);
            if (keepBits < kChunkBits) {
                freeMask &= (1ULL << keepBits) - 1;
            }
        }

        if (freeMask == 0) {
            continue;
        }

        unsigned bitIdx = static_cast<unsigned>(__builtin_ctzll(freeMask));
        RegionIndex candidate = static_cast<RegionIndex>(view.regionStart_ + wordRegionStart + bitIdx);
        if (candidate < viewEnd) {
            return candidate;
        }
    }
    return static_cast<RegionIndex>(committedRegionCount_);
}

bool FreeManager::PreciseFitFromLocked(RegionIndex start, RegionCount num) const
{
    if (start + num > committedRegionCount_) {
        return false;
    }
    for (RegionIndex idx = start; idx < start + num; ++idx) {
        if (!IsRegionFreeLocked(idx)) {
            return false;
        }
    }
    return true;
}

// Opt1: word-level fit check — no per-region view-lookup overhead.
// Verifies that all regions [localStart, localStart+num) are free within `view`.
bool FreeManager::PreciseFitInViewFromLocked(const OwnedSegmentView& view, size_t localStart, size_t num)
{
    if (num == 0) { return true; }
    size_t viewSize = static_cast<size_t>(view.regionEnd_ - view.regionStart_);
    if (localStart + num > viewSize) { return false; }
    size_t localEnd  = localStart + num;
    size_t startWord = localStart / kChunkBits;
    size_t endWord   = (localEnd - 1) / kChunkBits;
    for (size_t w = startWord; w <= endWord && w < view.bitmapWordCount_; ++w) {
        uint64_t word = view.allocBits_[w];
        if (word == 0) { continue; }
        size_t bitLo = (w == startWord) ? (localStart % kChunkBits) : 0;
        size_t bitHi = (w == endWord)   ? ((localEnd - 1) % kChunkBits + 1) : kChunkBits;
        uint64_t mask = (bitHi < kChunkBits)
            ? (((1ULL << bitHi) - 1) & (~0ULL << bitLo))
            : (~0ULL << bitLo);
        if (word & mask) { return false; }
    }
    return true;
}

// Find first free (allocBit=0) view-local position in [localPos, localEnd).
size_t FreeManager::FindFirstFreeLocalLocked(const OwnedSegmentView& view, size_t localPos, size_t localEnd)
{
    while (localPos < localEnd) {
        size_t w = localPos / kChunkBits;
        size_t b = localPos % kChunkBits;
        if (w >= view.bitmapWordCount_) { return localEnd; }
        uint64_t shifted = (~view.allocBits_[w]) >> b;
        if (shifted != 0) { return std::min(localPos + static_cast<size_t>(__builtin_ctzll(shifted)), localEnd); }
        localPos = (w + 1) * kChunkBits;
    }
    return localEnd;
}

// Find first allocated (allocBit=1) view-local position in [localPos, localEnd).
size_t FreeManager::FindFirstAllocLocalLocked(const OwnedSegmentView& view, size_t localPos, size_t localEnd)
{
    while (localPos < localEnd) {
        size_t w = localPos / kChunkBits;
        size_t b = localPos % kChunkBits;
        if (w >= view.bitmapWordCount_) { return localEnd; }
        uint64_t shifted = view.allocBits_[w] >> b;
        if (shifted != 0) { return std::min(localPos + static_cast<size_t>(__builtin_ctzll(shifted)), localEnd); }
        localPos = (w + 1) * kChunkBits;
    }
    return localEnd;
}

// Opt2: chunk-summary guided run search within a single view.
// Uses chunkSummaries_.max to skip chunks where no run of length `num` exists,
// then verifies candidates with PreciseFitInViewFromLocked (word-level, no view-lookup).
bool FreeManager::FindFreeRunInViewLocked(const OwnedSegmentView& view,
                                           size_t localStart, size_t localEnd,
                                           size_t num, size_t& localIdx)
{
    size_t viewSize = static_cast<size_t>(view.regionEnd_ - view.regionStart_);
    localEnd = std::min(localEnd, viewSize);
    if (localStart >= localEnd || localEnd - localStart < num) { return false; }

    size_t probe = localStart;
    while (probe + num <= localEnd) {
        // Chunk-summary skip: at chunk boundary, skip consecutive chunks with max < num.
        if (probe % kChunkBits == 0 && !view.chunkSummaries_.empty()) {
            size_t chunkIdx = probe / kChunkBits;
            while (chunkIdx < view.chunkSummaries_.size() &&
                   static_cast<size_t>(view.chunkSummaries_[chunkIdx].max) < num) {
                ++chunkIdx;
                probe = chunkIdx * kChunkBits;
                if (probe + num > localEnd) { return false; }
            }
        }

        // Find next free bit at or after probe.
        size_t candidate = FindFirstFreeLocalLocked(view, probe, localEnd);
        if (candidate + num > localEnd) { break; }

        // Word-level verify.
        if (PreciseFitInViewFromLocked(view, candidate, num)) {
            localIdx = candidate;
            return true;
        }

        // Find the first allocated bit in [candidate, candidate+num) and skip past it.
        size_t failBit   = FindFirstAllocLocalLocked(view, candidate, candidate + num);
        size_t afterFail = FindFirstFreeLocalLocked(view, failBit, localEnd);
        probe = afterFail;
    }
    return false;
}

bool FreeManager::TryTakeBorrowedRegionLocked(FreeManagerShard& shard, RegionIndex& idx)
{
    if (shard.borrowedNum_ == 0) {
        return false;
    }
    idx = shard.borrowedIdx_++;
    --shard.borrowedNum_;
    return true;
}

bool FreeManager::TryTakeOneRegionFromOwnedViewsLocked(FreeManagerShard& shard,
                                                       size_t startViewIdx,
                                                       RegionIndex startRegion,
                                                       RegionIndex& idx,
                                                       bool& reusedScavenged)
{
    size_t viewCount = shard.ownedViews_.size();
    if (viewCount == 0) {
        return false;
    }

    for (size_t offset = 0; offset < viewCount; ++offset) {
        size_t viewIdx = (startViewIdx + offset) % viewCount;
        OwnedSegmentView& view = shard.ownedViews_[viewIdx];
        RegionIndex probeStart = (offset == 0) ? startRegion : view.regionStart_;
        RegionIndex candidate = FindFirstFreeRegionInViewLocked(view, probeStart);
        if (candidate >= std::min(view.regionEnd_, static_cast<RegionIndex>(committedRegionCount_))) {
            continue;
        }

        MarkAllocatedOneLocked(shard, view, static_cast<size_t>(candidate - view.regionStart_), reusedScavenged);
        if (candidate >= shard.takeCursor_) {
            shard.takeCursor_ = FindNextOwnedFreeRegionLocked(shard, candidate + 1);
        }
        if (candidate == shard.reclaimCursor_) {
            shard.reclaimCursor_ = FindNextOwnedFreeRegionLocked(shard, candidate + 1);
        }

        shard.fastTakeHintValid_ = true;
        shard.fastTakeViewIdx_ = viewIdx;
        shard.fastTakeWordOffset_ = static_cast<size_t>(candidate - view.regionStart_) / kChunkBits;
        idx = candidate;
        return true;
    }

    return false;
}

bool FreeManager::TryTakeOneRegionFastLocked(FreeManagerShard& shard, RegionIndex& idx, bool& reusedScavenged)
{
    if (shard.ownedViews_.empty()) {
        return false;
    }

    if (shard.fastTakeHintValid_ && shard.fastTakeViewIdx_ < shard.ownedViews_.size()) {
        const OwnedSegmentView& hintedView = shard.ownedViews_[shard.fastTakeViewIdx_];
        RegionIndex hintedStart = hintedView.regionStart_ +
            static_cast<RegionIndex>(shard.fastTakeWordOffset_ * kChunkBits);
        hintedStart = std::max(hintedStart, std::max(hintedView.regionStart_, shard.takeCursor_));
        if (TryTakeOneRegionFromOwnedViewsLocked(shard, shard.fastTakeViewIdx_, hintedStart, idx, reusedScavenged)) {
            return true;
        }
        shard.fastTakeHintValid_ = false;
    }

    size_t startViewIdx = 0;
    RegionIndex takeStart = shard.takeCursor_;
    for (size_t viewIdx = 0; viewIdx < shard.ownedViews_.size(); ++viewIdx) {
        const OwnedSegmentView& view = shard.ownedViews_[viewIdx];
        if (takeStart < view.regionEnd_) {
            startViewIdx = viewIdx;
            takeStart = std::max(takeStart, view.regionStart_);
            break;
        }
    }
    if (TryTakeOneRegionFromOwnedViewsLocked(shard, startViewIdx, takeStart, idx, reusedScavenged)) {
        return true;
    }

    if (shard.reclaimCursor_ < shard.takeCursor_) {
        size_t reclaimViewIdx = 0;
        RegionIndex reclaimStart = shard.reclaimCursor_;
        for (size_t viewIdx = 0; viewIdx < shard.ownedViews_.size(); ++viewIdx) {
            const OwnedSegmentView& view = shard.ownedViews_[viewIdx];
            if (reclaimStart < view.regionEnd_) {
                reclaimViewIdx = viewIdx;
                reclaimStart = std::max(reclaimStart, view.regionStart_);
                break;
            }
        }
        if (TryTakeOneRegionFromOwnedViewsLocked(
                shard, reclaimViewIdx, reclaimStart, idx, reusedScavenged)) {
            return true;
        }
    }
    return false;
}

void FreeManager::MarkAllocatedOneLocked(FreeManagerShard& shard,
                                         OwnedSegmentView& view,
                                         size_t localIdx,
                                         bool& reusedScavenged)
{
    bool wasScavenged = TestBit(view.scavengedBits_.data(), localIdx);
    bool wasFree = !TestBit(view.allocBits_.data(), localIdx);
    reusedScavenged = wasScavenged;
    if (wasScavenged) {
        --shard.scavengedRegionCount_;
        ClearBit(view.scavengedBits_.data(), localIdx);
    }
    if (wasFree) {
        --shard.freeRegionCount_;
    }
    SetBit(view.allocBits_.data(), localIdx);
    UpdateChunkSummaryRangeLocked(view, localIdx, 1);  // also refreshes view.maxFreeRun_
}

bool FreeManager::FindFreeRunInRangeLocked(RegionIndex start, RegionIndex end, RegionCount num, RegionIndex& idx) const
{
    if (num == 0 || start >= end) {
        return false;
    }

    end = std::min(end, static_cast<RegionIndex>(committedRegionCount_));
    if (start >= end || static_cast<RegionCount>(end - start) < num) {
        return false;
    }

    for (RegionIndex probe = start; probe + num <= end; ++probe) {
        if (PreciseFitFromLocked(probe, num)) {
            idx = probe;
            return true;
        }
    }
    return false;
}

bool FreeManager::FindFreeRunLocked(RegionIndex cursor, RegionCount num, RegionIndex& idx) const
{
    if (num == 0 || num > committedRegionCount_) {
        return false;
    }

    cursor = std::min(cursor, static_cast<RegionIndex>(committedRegionCount_));
    for (RegionIndex probe = cursor; probe < committedRegionCount_;) {
        RegionIndex candidate = FindNextFreeRegionLocked(probe);
        if (candidate >= committedRegionCount_ || candidate + num > committedRegionCount_) {
            break;
        }
        if (PreciseFitFromLocked(candidate, num)) {
            idx = candidate;
            return true;
        }
        probe = candidate + 1;
    }
    return false;
}

bool FreeManager::FindOwnedFreeRunLocked(const FreeManagerShard& shard,
                                         RegionIndex cursor,
                                         RegionCount num,
                                         RegionIndex& idx) const
{
    if (num == 0 || num > committedRegionCount_ || shard.freeRegionCount_ < num) {
        return false;
    }
    if (segmentRegionCount_ == 0 || shard.ownedViews_.empty()) {
        return FindFreeRunLocked(cursor, num, idx);
    }

    cursor = std::min(cursor, static_cast<RegionIndex>(committedRegionCount_));

    if (num <= static_cast<RegionCount>(segmentRegionCount_)) {
        // Opt2: binary search for first view with regionEnd_ > cursor — skips O(log N) headers.
        // ownedViews_ is ordered by regionStart_ (segments always grow in address order).
        const auto& views = shard.ownedViews_;
        auto it = std::lower_bound(views.begin(), views.end(), cursor,
            [](const OwnedSegmentView& v, RegionIndex c) { return v.regionEnd_ <= c; });
        for (; it != views.end(); ++it) {
            const OwnedSegmentView& view = *it;
            if (view.regionStart_ >= static_cast<RegionIndex>(committedRegionCount_)) { break; }
            // Opt3: skip entire 64MB segment in O(1) when its cached max free run < num.
            if (static_cast<RegionCount>(view.maxFreeRun_) < num) { continue; }
            RegionIndex segmentEnd = std::min(view.regionEnd_, static_cast<RegionIndex>(committedRegionCount_));
            RegionIndex probeStart = std::max(cursor, view.regionStart_);
            size_t localStart = static_cast<size_t>(probeStart - view.regionStart_);
            size_t localEnd   = static_cast<size_t>(segmentEnd - view.regionStart_);
            size_t localIdx   = 0;
            if (FindFreeRunInViewLocked(view, localStart, localEnd, static_cast<size_t>(num), localIdx)) {
                idx = view.regionStart_ + static_cast<RegionIndex>(localIdx);
                return true;
            }
        }
        return false;
    }

    // Cross-segment search: merge runs of adjacent views into one contiguous span.
    // Binary search for the span containing cursor to avoid iterating from i=0.
    const auto& views = shard.ownedViews_;
    size_t n = views.size();
    size_t startI = static_cast<size_t>(
        std::lower_bound(views.begin(), views.end(), cursor,
            [](const OwnedSegmentView& v, RegionIndex c) { return v.regionEnd_ <= c; }) -
        views.begin());
    for (size_t i = startI; i < n; ) {
        size_t j = i;
        while (j + 1 < n && views[j].regionEnd_ == views[j + 1].regionStart_) {
            ++j;
        }
        RegionIndex spanStart = views[i].regionStart_;
        RegionIndex spanEnd = std::min(views[j].regionEnd_,
                                       static_cast<RegionIndex>(committedRegionCount_));
        if (spanStart < committedRegionCount_ && spanEnd > cursor) {
            RegionIndex probeStart = std::max(cursor, spanStart);
            if (static_cast<RegionCount>(spanEnd - probeStart) >= num &&
                FindFreeRunInRangeLocked(probeStart, spanEnd, num, idx)) {
                return true;
            }
        }
        i = j + 1;
    }
    return false;
}

FreeManager::RegionCount FreeManager::CountContiguousFreeInViewLocked(const OwnedSegmentView& view,
                                                                       RegionIndex start,
                                                                       RegionCount maxCount)
{
    if (maxCount == 0 || start >= view.regionEnd_) {
        return 0;
    }

    size_t localStart = static_cast<size_t>(start - view.regionStart_);
    size_t viewSize = static_cast<size_t>(view.regionEnd_ - view.regionStart_);
    RegionCount count = 0;
    size_t pos = localStart;

    while (count < maxCount && pos < viewSize) {
        size_t wordIdx = pos / kChunkBits;
        if (wordIdx >= view.bitmapWordCount_) {
            break;
        }
        size_t bitIdx = pos % kChunkBits;
        uint64_t shifted = view.allocBits_[wordIdx] >> bitIdx;
        size_t bitsLeft = kChunkBits - bitIdx;
        size_t need = static_cast<size_t>(maxCount) - count;
        size_t cap = std::min({need, bitsLeft, viewSize - pos});

        if (shifted == 0) {
            count += static_cast<RegionCount>(cap);
            pos += cap;
        } else {
            size_t free = static_cast<size_t>(__builtin_ctzll(shifted));
            count += static_cast<RegionCount>(std::min(free, cap));
            break;
        }
    }
    return count;
}

size_t FreeManager::ScavengeOwnedFreeRegionsLocked(FreeManagerShard& shard,
                                                   uint16_t ownerShardId,
                                                   size_t regionBudget)
{
    if (regionBudget == 0 || shard.freeRegionCount_ <= shard.scavengedRegionCount_) {
        return 0;
    }

    size_t scavengedRegions = 0;
    for (auto viewIt = shard.ownedViews_.rbegin();
         viewIt != shard.ownedViews_.rend() && scavengedRegions < regionBudget;
         ++viewIt) {
        OwnedSegmentView& view = *viewIt;
        size_t committedLocal = static_cast<size_t>(
            std::min(view.regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - view.regionStart_);
        if (committedLocal == 0) { continue; }

        size_t wordCount = (committedLocal + kChunkBits - 1) / kChunkBits;

        // Opt3: scan words backward; hotFree = free AND not-already-scavenged.
        // Track a pending run [pendingStart, pendingEnd) across word boundaries
        // to merge adjacent runs into one ReleaseUnits call.
        static constexpr size_t kNoPending = static_cast<size_t>(-1);
        size_t pendingStart = kNoPending;
        size_t pendingEnd   = 0;

        auto flushPending = [&]() {
            if (pendingStart == kNoPending) { return; }
            RegionCount run = static_cast<RegionCount>(pendingEnd - pendingStart);
            if (scavengedRegions + run > regionBudget) {
                size_t allowed = regionBudget - scavengedRegions;
                pendingStart = pendingEnd - allowed;
                run = static_cast<RegionCount>(allowed);
            }
            RegionIndex gs = view.regionStart_ + static_cast<RegionIndex>(pendingStart);
            ReleaseUnits(RegionToUnitIdx(gs), RegionCountToUnitCount(run));
            MarkScavengedLocked(shard, gs, run);
            scavengedRegions += run;
            pendingStart = kNoPending;
        };

        for (size_t wi = wordCount; wi-- > 0 && scavengedRegions < regionBudget; ) {
            size_t wordBase = wi * kChunkBits;
            size_t wordBits = std::min(committedLocal - wordBase, kChunkBits);
            uint64_t hot = (~view.allocBits_[wi]) & (~view.scavengedBits_[wi]);
            if (wordBits < kChunkBits) { hot &= (1ULL << wordBits) - 1; }
            if (hot == 0) {
                // No hot-free in this word; flush pending (gap found).
                flushPending();
                continue;
            }

            // Extract runs of consecutive 1s in `hot`, highest first.
            while (hot != 0 && scavengedRegions < regionBudget) {
                int hi = 63 - __builtin_clzll(hot);
                // Find start of contiguous run ending at bit hi:
                // highest 0-bit strictly below hi in hot → lo = that position + 1
                uint64_t below = hot & ((static_cast<size_t>(hi) > 0) ? ((1ULL << hi) - 1) : 0);
                uint64_t gaps  = (~hot) & ((static_cast<size_t>(hi) > 0) ? ((1ULL << hi) - 1) : 0);
                int lo = (gaps == 0) ? 0 : (63 - __builtin_clzll(gaps) + 1);

                size_t runEnd   = wordBase + static_cast<size_t>(hi) + 1;
                size_t runStart = wordBase + static_cast<size_t>(lo);

                // Merge with pending if adjacent (this run ends where pending starts).
                if (pendingStart != kNoPending && runEnd == pendingStart) {
                    pendingStart = runStart;
                } else {
                    flushPending();
                    if (scavengedRegions >= regionBudget) { break; }
                    pendingStart = runStart;
                    pendingEnd   = runEnd;
                }

                // Clear processed bits.
                int runLen = hi - lo + 1;
                uint64_t clearMask = (runLen < 64)
                    ? (((1ULL << runLen) - 1) << lo)
                    : ~0ULL;
                hot &= ~clearMask;
            }
        }
        flushPending();
    }
    return scavengedRegions;
}

void FreeManager::AppendGroupedOwnerSpansLocked(std::vector<std::vector<RegionSpan>>& groupedSpans,
                                                RegionIndex idx,
                                                RegionCount num) const
{
    if (num == 0) {
        return;
    }

    if (segmentRegionCount_ == 0 || segmentOwnerShard_.empty()) {
        groupedSpans[0].push_back({ idx, num });
        return;
    }

    RegionIndex cursor = idx;
    RegionCount remaining = num;
    RegionIndex committedLimit = static_cast<RegionIndex>(committedRegionCount_);
    while (remaining != 0 && cursor < committedLimit) {
        size_t segmentIdx = static_cast<size_t>(cursor) / segmentRegionCount_;
        RegionIndex segmentEnd = static_cast<RegionIndex>(
            std::min(committedRegionCount_, (segmentIdx + 1) * segmentRegionCount_));
        if (segmentEnd <= cursor) {
            break;
        }
        RegionCount chunk = std::min<RegionCount>(remaining, static_cast<RegionCount>(segmentEnd - cursor));
        uint16_t ownerShardId = segmentIdx < segmentOwnerShard_.size() ? segmentOwnerShard_[segmentIdx] : 0;
        groupedSpans[ownerShardId].push_back({ cursor, chunk });
        cursor = static_cast<RegionIndex>(cursor + chunk);
        remaining = static_cast<RegionCount>(remaining - chunk);
    }
}

void FreeManager::ApplyGroupedOwnerReclaim(std::vector<std::vector<RegionSpan>>& groupedSpans)
{
    for (uint16_t shardId = 0; shardId < shardCount_; ++shardId) {
        if (groupedSpans[shardId].empty()) {
            continue;
        }
        FreeManagerShard& shardState = GetShard(shardId);
        std::lock_guard<std::mutex> shardLock(shardState.shardMutex_);
        RegionIndex reclaimMin = shardState.reclaimCursor_;
        for (const RegionSpan& span : groupedSpans[shardId]) {
            MarkFreeLocked(shardState, span.idx, span.num);
            if (span.idx < shardState.takeCursor_) {
                reclaimMin = std::min(reclaimMin, span.idx);
            }
        }
        if (reclaimMin < shardState.takeCursor_) {
            shardState.reclaimCursor_ = std::min(shardState.reclaimCursor_, reclaimMin);
        }
    }
}

bool FreeManager::TryStealSingleRegionBatchLocked(uint16_t receiverShardId,
                                                  RegionIndex& idx,
                                                  RegionSpan& reusedSpan)
{
    if (receiverShardId >= shardCount_ || shardCount_ <= 1) {
        return false;
    }

    FreeManagerShard& receiverShard = GetShard(receiverShardId);

    for (uint32_t attempt = 0; attempt < kMaxSingleRegionStealAttempts; ++attempt) {
        // Change 3: lock-free donor selection via atomic fetch_add (eliminates two receiverLock sections)
        uint16_t raw = receiverShard.nextStealDonorShard_.fetch_add(1, std::memory_order_relaxed);
        uint16_t donorShardId = static_cast<uint16_t>(raw % shardCount_);
        if (donorShardId == receiverShardId) {
            donorShardId = static_cast<uint16_t>((static_cast<uint32_t>(donorShardId) + 1) % shardCount_);
        }
        if (donorShardId == receiverShardId) {
            return false;
        }

        RegionIndex donorIdx = 0;
        RegionCount batchCount = 0;
        bool batchReusedScavenged = false;
        {
            FreeManagerShard& donorShard = GetShard(donorShardId);
            std::lock_guard<std::mutex> donorLock(donorShard.shardMutex_);

            // Change 4: use fastTakeHint to guide first-region search; use efficient bitmap scan
            bool fromTakeSide = false;
            bool fromReclaimSide = false;
            RegionIndex candidate = static_cast<RegionIndex>(committedRegionCount_);

            if (donorShard.fastTakeHintValid_ &&
                donorShard.fastTakeViewIdx_ < donorShard.ownedViews_.size()) {
                const OwnedSegmentView& hintView = donorShard.ownedViews_[donorShard.fastTakeViewIdx_];
                RegionIndex hintPos = hintView.regionStart_ +
                    static_cast<RegionIndex>(donorShard.fastTakeWordOffset_ * kChunkBits);
                RegionIndex hintedStart = std::max(donorShard.takeCursor_, hintPos);
                candidate = FindFirstFreeRegionInViewLocked(hintView, hintedStart);
            }
            if (candidate >= committedRegionCount_) {
                candidate = FindNextOwnedFreeRegionLocked(donorShard, donorShard.takeCursor_);
            }
            if (candidate < committedRegionCount_) {
                fromTakeSide = true;
                donorIdx = candidate;
            } else if (donorShard.reclaimCursor_ < donorShard.takeCursor_) {
                candidate = FindNextOwnedFreeRegionLocked(donorShard, donorShard.reclaimCursor_);
                if (candidate < committedRegionCount_) {
                    fromReclaimSide = true;
                    donorIdx = candidate;
                }
            }

            if (!fromTakeSide && !fromReclaimSide) {
                continue;
            }

            // Change 5: bitmap-based contiguous count via ctzll (O(1) for kSingleRegionStealBatch=4)
            OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(donorShard, donorIdx);
            if (view == nullptr) {
                continue;
            }
            batchCount = CountContiguousFreeInViewLocked(*view, donorIdx, kSingleRegionStealBatch);
            if (batchCount == 0) {
                continue;
            }

            MarkAllocatedLocked(donorShard, donorIdx, batchCount, batchReusedScavenged);
            if (fromTakeSide && donorIdx == donorShard.takeCursor_) {
                donorShard.takeCursor_ = FindNextOwnedFreeRegionLocked(donorShard, donorIdx + batchCount);
            }
            if (fromReclaimSide && donorIdx == donorShard.reclaimCursor_) {
                donorShard.reclaimCursor_ = FindNextOwnedFreeRegionLocked(donorShard, donorIdx + batchCount);
            }
        }

        idx = donorIdx;
        // Change 3+6: write directly to receiver's inline borrowed fields (no receiverLock needed)
        if (batchCount > 1) {
            receiverShard.borrowedIdx_ = static_cast<RegionIndex>(donorIdx + 1);
            receiverShard.borrowedNum_ = static_cast<RegionCount>(batchCount - 1);
        }
        if (batchReusedScavenged) {
            reusedSpan.idx = donorIdx;
            reusedSpan.num = batchCount;
        }
        return true;
    }

    return false;
}

size_t FreeManager::CountScavengedInViewRangeLocked(const OwnedSegmentView& view, size_t localStart, RegionCount num) const
{
    size_t count = 0;
    for (size_t localIdx = localStart; localIdx < localStart + num; ++localIdx) {
        if (TestBit(view.scavengedBits_.data(), localIdx)) {
            ++count;
        }
    }
    return count;
}

size_t FreeManager::CountFreeInViewRangeLocked(const OwnedSegmentView& view, size_t localStart, RegionCount num) const
{
    size_t count = 0;
    for (size_t localIdx = localStart; localIdx < localStart + num; ++localIdx) {
        if (!TestBit(view.allocBits_.data(), localIdx)) {
            ++count;
        }
    }
    return count;
}

void FreeManager::MarkAllocatedLocked(FreeManagerShard& shard,
                                      RegionIndex idx,
                                      RegionCount num,
                                      bool& reusedScavenged)
{
    if (num == 1) {
        OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shard, idx);
        CHECK_DETAIL(view != nullptr, "[MS-GC] single alloc target view missing: region=%u", idx);
        MarkAllocatedOneLocked(shard, *view, static_cast<size_t>(idx - view->regionStart_), reusedScavenged);
        return;
    }

    reusedScavenged = false;
    RegionIndex cursor = idx;
    RegionCount remaining = num;
    while (remaining != 0) {
        OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shard, cursor);
        CHECK_DETAIL(view != nullptr, "[MS-GC] alloc target view missing: region=%u", cursor);
        RegionCount chunk = std::min<RegionCount>(remaining, static_cast<RegionCount>(view->regionEnd_ - cursor));
        size_t localStart = static_cast<size_t>(cursor - view->regionStart_);
        size_t scavengedCount = CountScavengedInViewRangeLocked(*view, localStart, chunk);
        reusedScavenged = reusedScavenged || (scavengedCount != 0);
        shard.scavengedRegionCount_ -= scavengedCount;
        shard.freeRegionCount_ -= CountFreeInViewRangeLocked(*view, localStart, chunk);
        for (size_t localIdx = localStart; localIdx < localStart + chunk; ++localIdx) {
            SetBit(view->allocBits_.data(), localIdx);
            ClearBit(view->scavengedBits_.data(), localIdx);
        }
        UpdateChunkSummaryRangeLocked(*view, localStart, chunk);  // also refreshes view.maxFreeRun_
        cursor = static_cast<RegionIndex>(cursor + chunk);
        remaining = static_cast<RegionCount>(remaining - chunk);
    }
}

void FreeManager::MarkFreeLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num)
{
    RegionIndex cursor = idx;
    RegionCount remaining = num;
    while (remaining != 0 && cursor < committedRegionCount_) {
        OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shard, cursor);
        CHECK_DETAIL(view != nullptr, "[MS-GC] free target view missing: region=%u", cursor);
        RegionCount chunk = std::min<RegionCount>(
            remaining,
            static_cast<RegionCount>(std::min<RegionIndex>(view->regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - cursor));
        size_t localStart = static_cast<size_t>(cursor - view->regionStart_);
        // Word-level bitmap update: O(num/64) popcount-based, no per-region branches.
        size_t localEnd = localStart + chunk;
        size_t wordStart = localStart / kChunkBits;
        size_t wordEnd = (localEnd - 1) / kChunkBits;
        for (size_t wi = wordStart; wi <= wordEnd && wi < view->bitmapWordCount_; ++wi) {
            size_t bitLo = (wi == wordStart) ? (localStart % kChunkBits) : 0;
            size_t bitHi = (wi == wordEnd) ? ((localEnd - 1) % kChunkBits + 1) : kChunkBits;
            // Handle partial last word within view boundary.
            size_t viewWordLimit = std::min(kChunkBits,
                static_cast<size_t>(view->regionEnd_ - view->regionStart_) - wi * kChunkBits);
            bitHi = std::min(bitHi, viewWordLimit);
            if (bitLo >= bitHi) { continue; }

            uint64_t mask = (bitHi < kChunkBits)
                ? (((1ULL << bitHi) - 1) & (~0ULL << bitLo))
                : (~0ULL << bitLo);

            uint64_t oldAlloc = view->allocBits_[wi];
            uint64_t oldScav = view->scavengedBits_[wi];

            shard.freeRegionCount_ += __builtin_popcountll(oldAlloc & mask);
            shard.scavengedRegionCount_ -= __builtin_popcountll(oldScav & mask);

            view->allocBits_[wi] = oldAlloc & ~mask;
            view->scavengedBits_[wi] = oldScav & ~mask;
        }
        UpdateChunkSummaryRangeLocked(*view, localStart, chunk);  // also refreshes view.maxFreeRun_
        cursor = static_cast<RegionIndex>(cursor + chunk);
        remaining = static_cast<RegionCount>(remaining - chunk);
    }
}

void FreeManager::MarkScavengedLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num)
{
    RegionIndex cursor = idx;
    RegionCount remaining = num;
    while (remaining != 0 && cursor < committedRegionCount_) {
        OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shard, cursor);
        CHECK_DETAIL(view != nullptr, "[MS-GC] scavenge target view missing: region=%u", cursor);
        RegionCount chunk = std::min<RegionCount>(
            remaining,
            static_cast<RegionCount>(std::min<RegionIndex>(view->regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - cursor));
        size_t localStart = static_cast<size_t>(cursor - view->regionStart_);
        for (size_t localIdx = localStart; localIdx < localStart + chunk; ++localIdx) {
            if (!TestBit(view->allocBits_.data(), localIdx) && !TestBit(view->scavengedBits_.data(), localIdx)) {
                SetBit(view->scavengedBits_.data(), localIdx);
                ++shard.scavengedRegionCount_;
            }
        }
        cursor = static_cast<RegionIndex>(cursor + chunk);
        remaining = static_cast<RegionCount>(remaining - chunk);
    }
    uint32_t hotFree = static_cast<uint32_t>(
        shard.freeRegionCount_ >= shard.scavengedRegionCount_
            ? (shard.freeRegionCount_ - shard.scavengedRegionCount_) : 0);
    shard.snapHotFreeRegions_.store(hotFree, std::memory_order_relaxed);
}

void FreeManager::MarkUnavailableLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num)
{
    RegionIndex cursor = idx;
    RegionCount remaining = num;
    while (remaining != 0) {
        OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shard, cursor);
        CHECK_DETAIL(view != nullptr, "[MS-GC] unavailable target view missing: region=%u", cursor);
        RegionCount chunk = std::min<RegionCount>(remaining, static_cast<RegionCount>(view->regionEnd_ - cursor));
        size_t localStart = static_cast<size_t>(cursor - view->regionStart_);
        shard.scavengedRegionCount_ -= CountScavengedInViewRangeLocked(*view, localStart, chunk);
        shard.freeRegionCount_ -= CountFreeInViewRangeLocked(*view, localStart, chunk);
        for (size_t localIdx = localStart; localIdx < localStart + chunk; ++localIdx) {
            SetBit(view->allocBits_.data(), localIdx);
            ClearBit(view->scavengedBits_.data(), localIdx);
        }
        UpdateChunkSummaryRangeLocked(*view, localStart, chunk);  // also refreshes view.maxFreeRun_
        cursor = static_cast<RegionIndex>(cursor + chunk);
        remaining = static_cast<RegionCount>(remaining - chunk);
    }
}

// Opt4: word-level ops — O(1) prefix/suffix, O(num_free_runs) max (≤32).
void FreeManager::UpdateChunkSummaryLocked(OwnedSegmentView& view, size_t chunkIdx)
{
    size_t regionCount = static_cast<size_t>(view.regionEnd_ - view.regionStart_);
    size_t chunkBase   = chunkIdx * kChunkBits;
    size_t chunkBits   = std::min(regionCount - chunkBase, kChunkBits);

    uint64_t word = (chunkIdx < view.bitmapWordCount_) ? view.allocBits_[chunkIdx] : ~0ULL;
    // Mask out bits beyond the valid range; treat them as allocated.
    uint64_t mask = (chunkBits < kChunkBits) ? ((1ULL << chunkBits) - 1) : ~0ULL;
    word &= mask;

    ChunkSummary summary {};
    if (word == 0) {
        // Entire chunk is free.
        auto bits = static_cast<uint16_t>(chunkBits);
        summary.start = summary.end = summary.max = bits;
    } else {
        // prefix = ctz(word): first allocated bit position = number of free bits before it.
        summary.start = static_cast<uint16_t>(__builtin_ctzll(word));
        // suffix = number of free bits after the last allocated bit.
        int highAlloc = 63 - __builtin_clzll(word);
        summary.end = static_cast<uint16_t>(chunkBits - 1 - static_cast<size_t>(highAlloc));
        // max: longest run of 0-bits in `word` (within valid range).
        uint64_t freeWord = (~word) & mask;
        uint16_t best = 0;
        while (freeWord != 0) {
            int lo = __builtin_ctzll(freeWord);
            uint64_t shifted = freeWord >> lo;
            int runLen = __builtin_ctzll(~shifted);  // consecutive 1s in freeWord
            if (runLen > best) { best = static_cast<uint16_t>(runLen); }
            freeWord &= ~(((runLen < 64) ? ((1ULL << runLen) - 1) : ~0ULL) << lo);
        }
        summary.max = best;
    }
    view.chunkSummaries_[chunkIdx] = summary;
}

void FreeManager::UpdateChunkSummaryRangeLocked(OwnedSegmentView& view, size_t localStart, RegionCount num)
{
    if (view.chunkSummaries_.empty()) {
        return;
    }
    size_t firstChunk = localStart / kChunkBits;
    size_t lastChunk = (localStart + std::max<RegionCount>(1, num) - 1) / kChunkBits;
    for (size_t chunkIdx = firstChunk; chunkIdx <= lastChunk && chunkIdx < view.chunkSummaries_.size(); ++chunkIdx) {
        UpdateChunkSummaryLocked(view, chunkIdx);
    }
    // Opt3: recompute per-view max free run so FindOwnedFreeRunLocked can skip segments cheaply.
    view.maxFreeRun_ = ComputeMaxFreeRunInViewLocked(view);
}

// Opt5: use chunkSummaries_ for O(num_chunks) cross-chunk aggregation instead of O(regionCount).
// Tracks a "streak" of free regions ending at the boundary between consecutive chunks.
uint32_t FreeManager::ComputeMaxFreeRunInViewLocked(const OwnedSegmentView& view) const
{
    size_t regionCount = static_cast<size_t>(view.regionEnd_ - view.regionStart_);
    if (regionCount == 0) { return 0; }

    if (view.chunkSummaries_.empty()) {
        // Fallback: per-bit scan (should only happen during initialisation before summaries exist).
        uint32_t best = 0, cur = 0;
        for (size_t i = 0; i < regionCount; ++i) {
            if (!TestBit(view.allocBits_.data(), i)) { ++cur; best = std::max(best, cur); }
            else { cur = 0; }
        }
        return best;
    }

    uint32_t best   = 0;
    uint32_t streak = 0;  // free run that extends from the end of the previous chunk
    for (size_t c = 0; c < view.chunkSummaries_.size(); ++c) {
        const ChunkSummary& cs = view.chunkSummaries_[c];
        size_t chunkBits = std::min(regionCount - c * kChunkBits, kChunkBits);

        // Run crossing the boundary: streak from previous chunk + free prefix of this chunk.
        uint32_t crossRun = streak + static_cast<uint32_t>(cs.start);
        best = std::max(best, crossRun);
        best = std::max(best, static_cast<uint32_t>(cs.max));

        if (static_cast<size_t>(cs.start) == chunkBits) {
            // Entire chunk is free: extend the streak through it.
            streak += static_cast<uint32_t>(chunkBits);
        } else {
            // Streak resets to the free suffix of this chunk.
            streak = static_cast<uint32_t>(cs.end);
        }
    }
    return best;
}

// Opt1: reads view.maxFreeRun_ (O(N)) instead of recomputing O(N×2048).
// Only called from cold paths (stats, initialize, reclaim).
void FreeManager::RefreshShardMaxOwnedFreeRunLocked(FreeManagerShard& shard)
{
    uint32_t maxRun = 0;
    for (const OwnedSegmentView& view : shard.ownedViews_) {
        maxRun = std::max(maxRun, view.maxFreeRun_);
    }
    shard.maxOwnedFreeRun_ = maxRun;
    shard.snapMaxFreeRun_.store(maxRun, std::memory_order_relaxed);
    uint32_t hotFree = static_cast<uint32_t>(
        shard.freeRegionCount_ >= shard.scavengedRegionCount_
            ? (shard.freeRegionCount_ - shard.scavengedRegionCount_) : 0);
    shard.snapHotFreeRegions_.store(hotFree, std::memory_order_relaxed);
}

size_t FreeManager::CountTrailingFreeRegionsLocked() const
{
    size_t count = 0;
    RegionIndex idx = static_cast<RegionIndex>(committedRegionCount_);
    while (idx > 0 && IsRegionFreeLocked(idx - 1)) {
        --idx;
        ++count;
    }
    return count;
}

void FreeManager::ReleaseUnits(UnitIndex idx, UnitCount num) const
{
    uintptr_t addr = heapStart_ + static_cast<uintptr_t>(idx) * unitSize_;
    size_t size = static_cast<size_t>(num) * unitSize_;
#if defined(_WIN64)
    CHECK_E(UNLIKELY(!VirtualFree(reinterpret_cast<void*>(addr), size, MEM_DECOMMIT)),
            "VirtualFree failed in FreeManager::ReleaseUnits");
#elif defined(__APPLE__)
    MemorySet(addr, size, 0, size);
    void* ret = mmap(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ret == MAP_FAILED) {
        LOG(RTLOG_ERROR, "[MS-GC] FreeManager::ReleaseUnits mmap fixed failed");
    }
#else
    (void)madvise(reinterpret_cast<void*>(addr), size, MADV_DONTNEED);
#endif
}

void FreeManager::ReuseUnits(UnitIndex idx, UnitCount num) const
{
#if defined(_WIN64)
    uintptr_t addr = heapStart_ + static_cast<uintptr_t>(idx) * unitSize_;
    size_t size = static_cast<size_t>(num) * unitSize_;
    void* ret = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_COMMIT, PAGE_READWRITE);
    CHECK_DETAIL(ret != nullptr, "VirtualAlloc failed in FreeManager::ReuseUnits");
#else
    (void)idx;
    (void)num;
#endif
}

FreeManager::UnitIndex FreeManager::RegionToUnitIdx(RegionIndex regionIdx) const
{
    CheckAlignmentConfig();
    return static_cast<UnitIndex>(static_cast<size_t>(regionIdx) * unitsPerRegion_);
}

FreeManager::UnitCount FreeManager::RegionCountToUnitCount(RegionCount regionCount) const
{
    CheckAlignmentConfig();
    return static_cast<UnitCount>(static_cast<size_t>(regionCount) * unitsPerRegion_);
}

FreeManager::RegionIndex FreeManager::UnitToRegionIdx(UnitIndex unitIdx) const
{
    CheckAlignmentConfig();
    CHECK_DETAIL(unitIdx % static_cast<UnitIndex>(unitsPerRegion_) == 0,
                 "[MS-GC] unit index is not region aligned: unitIdx=%u, unitsPerRegion=%zu",
                 unitIdx, unitsPerRegion_);
    return static_cast<RegionIndex>(static_cast<size_t>(unitIdx) / unitsPerRegion_);
}

FreeManager::RegionCount FreeManager::UnitCountToRegionCount(UnitCount unitCount) const
{
    CheckAlignmentConfig();
    CHECK_DETAIL(unitCount % static_cast<UnitCount>(unitsPerRegion_) == 0,
                 "[MS-GC] unit count is not region aligned: unitCount=%u, unitsPerRegion=%zu",
                 unitCount, unitsPerRegion_);
    return static_cast<RegionCount>(static_cast<size_t>(unitCount) / unitsPerRegion_);
}

void FreeManager::CheckAlignmentConfig() const
{
    CHECK_DETAIL(unitSize_ != 0, "[MS-GC] FreeManager unitSize must not be zero");
    CHECK_DETAIL(unitsPerRegion_ != 0, "[MS-GC] FreeManager unitsPerRegion must not be zero");
}

size_t FreeManager::GetRegionSizeBytes() const
{
    CheckAlignmentConfig();
    return unitSize_ * unitsPerRegion_;
}

FreeManager::RegionIndex FreeManager::AddrToRegionIdx(uintptr_t addr) const
{
    CHECK_DETAIL(addr >= heapStart_, "[MS-GC] address is below heap start: addr=0x%zx, heapStart=0x%zx",
                 addr, heapStart_);
    uintptr_t delta = addr - heapStart_;
    size_t regionSize = GetRegionSizeBytes();
    CHECK_DETAIL(delta % regionSize == 0,
                 "[MS-GC] address is not region aligned: addr=0x%zx, heapStart=0x%zx, regionSize=%zu",
                 addr, heapStart_, regionSize);
    return static_cast<RegionIndex>(delta / regionSize);
}

uintptr_t FreeManager::RegionIdxToAddr(RegionIndex regionIdx) const
{
    return heapStart_ + static_cast<uintptr_t>(regionIdx) * GetRegionSizeBytes();
}

uint16_t FreeManager::ResolveOwnerShardId(RegionIndex idx) const
{
    std::lock_guard<std::mutex> lg(pageHeapMutex_);
    return ResolveOwnerShardIdLocked(idx);
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
