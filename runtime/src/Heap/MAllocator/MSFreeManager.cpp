// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSFreeManager.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <thread>

#ifndef _WIN64
#include <sys/mman.h>
#else
#include <windows.h>
#endif

#include "Base/Log.h"
#include "Base/MemUtils.h"
#include "Heap/MCollector/MSHeapAccounting.h"
#include "Heap/MCollector/MSMemoryLimitPolicy.h"

namespace MapleRuntime {

constexpr size_t FreeManager::kChunkBits;

uint64_t FreeManager::ValidRegionMaskForWord(size_t wordIdx, size_t committedLocal)
{
    size_t wordStart = wordIdx * kChunkBits;
    if (wordStart >= committedLocal) {
        return 0;
    }
    size_t validBits = std::min(kChunkBits, committedLocal - wordStart);
    return validBits == kChunkBits ? ~0ULL : ((1ULL << validBits) - 1);
}

uint64_t FreeManager::HotFreeMaskForWord(const OwnedSegmentView& view,
                                         size_t wordIdx,
                                         size_t committedLocal)
{
    if (wordIdx >= view.bitmapWordCount_) {
        return 0;
    }
    return (~view.allocBits_[wordIdx]) & (~view.scavengedBits_[wordIdx]) &
           ValidRegionMaskForWord(wordIdx, committedLocal);
}

void FreeManager::RefreshHotWordLocked(OwnedSegmentView& view,
                                       size_t wordIdx,
                                       size_t committedLocal)
{
    CHECK_DETAIL(wordIdx < kChunkBits,
                 "[MS-GC] Scavenge hot-word index exceeds summary capacity: word=%zu",
                 wordIdx);
    uint64_t wordBit = 1ULL << wordIdx;
    if (HotFreeMaskForWord(view, wordIdx, committedLocal) != 0) {
        view.hotWordMask_ |= wordBit;
    } else {
        view.hotWordMask_ &= ~wordBit;
    }
}

void FreeManager::RefreshHotWordsInRangeLocked(OwnedSegmentView& view,
                                               size_t localStart,
                                               RegionCount num,
                                               size_t committedLocal)
{
    if (num == 0 || view.bitmapWordCount_ == 0) {
        return;
    }
    size_t firstWord = localStart / kChunkBits;
    size_t lastWord = (localStart + static_cast<size_t>(num) - 1) / kChunkBits;
    for (size_t wordIdx = firstWord;
         wordIdx <= lastWord && wordIdx < view.bitmapWordCount_;
         ++wordIdx) {
        RefreshHotWordLocked(view, wordIdx, committedLocal);
    }
}

FreeManager::OwnedSegmentView FreeManager::BuildOwnedSegmentView(size_t segmentIdx, RegionCount regionCount) const
{
    OwnedSegmentView view {};
    view.segmentIdx_ = segmentIdx;
    view.regionStart_ = static_cast<RegionIndex>(segmentIdx * segmentRegionCount_);
    view.regionEnd_ = static_cast<RegionIndex>(view.regionStart_ + regionCount);
    view.bitmapWordCount_ = RegionCountToChunkCount(regionCount);
    CHECK_DETAIL(view.bitmapWordCount_ <= kChunkBits,
                 "[MS-GC] Scavenge hot-word summary supports at most 64 bitmap words per segment");
    view.summaryChunkCount_ = RegionCountToChunkCount(regionCount);
    view.allocBits_.assign(view.bitmapWordCount_, 0);
    view.scavengedBits_.assign(view.bitmapWordCount_, 0);
    view.scavengingBits_.assign(view.bitmapWordCount_, 0);
    view.freeEpochs_.assign(
        regionCount, scavengeEpoch_.load(std::memory_order_relaxed));
    view.chunkSummaries_.assign(view.summaryChunkCount_, {});
    view.maxFreeRun_ = regionCount;  // freshly grown: entirely free
    RefreshHotWordsInRangeLocked(view, 0, regionCount, regionCount);
    return view;
}

void FreeManager::AttachOwnedSegmentLocked(FreeManagerShard& shard, size_t segmentIdx, RegionCount regionCount)
{
    CHECK_DETAIL(shard.ownedViews_.empty() ||
                     shard.ownedViews_.back().segmentIdx_ < segmentIdx,
                 "[MS-GC] owned segment views must be appended in segment order: previous=%zu current=%zu",
                 shard.ownedViews_.empty() ? 0 : shard.ownedViews_.back().segmentIdx_,
                 segmentIdx);
    OwnedSegmentView view = BuildOwnedSegmentView(segmentIdx, regionCount);
    shard.freeRegionCount_ += regionCount;
    shard.ownedViews_.push_back(view);
    shard.fastTakeHintValid_ = false;
    shard.scavengeViewCursor_ = shard.ownedViews_.size() - 1;
    shard.scavengeWordCursor_ = view.bitmapWordCount_ == 0 ? 0 : view.bitmapWordCount_ - 1;
    RefreshRegionSnapshotsLocked(shard);
}

void FreeManager::DetachOwnedSegmentLocked(FreeManagerShard& shard)
{
    if (shard.ownedViews_.empty()) {
        return;
    }

    shard.ownedViews_.pop_back();
    shard.fastTakeHintValid_ = false;
    if (shard.ownedViews_.empty()) {
        shard.scavengeViewCursor_ = 0;
        shard.scavengeWordCursor_ = 0;
    } else {
        shard.scavengeViewCursor_ %= shard.ownedViews_.size();
        size_t wordCount = shard.ownedViews_[shard.scavengeViewCursor_].bitmapWordCount_;
        shard.scavengeWordCursor_ = wordCount == 0 ? 0 : std::min(shard.scavengeWordCursor_, wordCount - 1);
    }
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

void FreeManager::PublishCommittedTopology(size_t committedRegionCount, uintptr_t committedEnd)
{
    // Region views and their owner table entries must be initialized before a
    // reader can observe this boundary. Publish the count last so readers that
    // acquire it also observe the preceding topology writes.
    committedEnd_.store(committedEnd, std::memory_order_release);
    committedRegionCount_.store(committedRegionCount, std::memory_order_release);
}

std::string FreeManager::DescribeCommittedShardSegmentsLocked(
    size_t committedSegmentCount) const
{
    std::vector<size_t> shardSegments(shardCount_, 0);
    size_t segmentLimit = std::min(committedSegmentCount, segmentOwnerShard_.size());
    for (size_t segmentIdx = 0; segmentIdx < segmentLimit; ++segmentIdx) {
        uint16_t ownerShardId = segmentOwnerShard_[segmentIdx];
        if (ownerShardId < shardSegments.size()) {
            ++shardSegments[ownerShardId];
        }
    }

    std::ostringstream stream;
    stream << '[';
    for (size_t shardIdx = 0; shardIdx < shardSegments.size(); ++shardIdx) {
        if (shardIdx != 0) {
            stream << ',';
        }
        stream << shardIdx << ':' << shardSegments[shardIdx];
    }
    stream << ']';
    return stream.str();
}

bool FreeManager::FindNonEmptyShardInRange(uint16_t receiverShardId,
                                           uint32_t begin,
                                           uint32_t end,
                                           uint16_t& donorShardId) const
{
    if (begin >= end || end > shardCount_ || nonEmptyShardWords_ == nullptr) {
        return false;
    }

    size_t firstWord = begin / kChunkBits;
    size_t lastWord = (end - 1) / kChunkBits;
    for (size_t wordIdx = firstWord; wordIdx <= lastWord; ++wordIdx) {
        uint64_t candidates = nonEmptyShardWords_[wordIdx].load(std::memory_order_acquire);
        if (wordIdx == firstWord) {
            candidates &= (~0ULL << (begin % kChunkBits));
        }
        if (wordIdx == lastWord && end % kChunkBits != 0) {
            candidates &= (1ULL << (end % kChunkBits)) - 1;
        }
        size_t receiverWord = static_cast<size_t>(receiverShardId) / kChunkBits;
        if (wordIdx == receiverWord) {
            candidates &= ~(1ULL << (receiverShardId % kChunkBits));
        }
        if (candidates == 0) {
            continue;
        }
        uint32_t candidate = static_cast<uint32_t>(
            wordIdx * kChunkBits + static_cast<size_t>(__builtin_ctzll(candidates)));
        if (candidate < end && candidate < shardCount_) {
            donorShardId = static_cast<uint16_t>(candidate);
            return true;
        }
    }
    return false;
}

bool FreeManager::FindNextNonEmptyDonorShard(uint16_t receiverShardId,
                                             uint32_t searchStart,
                                             uint16_t& donorShardId) const
{
    if (shardCount_ <= 1 || receiverShardId >= shardCount_) {
        return false;
    }
    uint32_t normalizedStart = searchStart % shardCount_;
    return FindNonEmptyShardInRange(receiverShardId, normalizedStart, shardCount_, donorShardId) ||
        FindNonEmptyShardInRange(receiverShardId, 0, normalizedStart, donorShardId);
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

void FreeManager::Initialize(size_t unitSize, size_t unitsPerRegion, RegionCount committedRegionCount,
                             RegionCount maxVirtualRegionCount,
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
    committedEnd_.store(committedEnd, std::memory_order_relaxed);
    maxHeapEnd_ = maxHeapEnd;
    committedRegionCount_.store(static_cast<size_t>(committedRegionCount), std::memory_order_relaxed);
    maxVirtualRegionCount_ = static_cast<size_t>(maxVirtualRegionCount);
    segmentSize_ = segmentSize;
    shardCount_ = NormalizeMSShardCount(shardCount);
    segmentRegionCount_ = segmentSize_ / GetRegionSizeBytes();
    CHECK_DETAIL(segmentRegionCount_ != 0,
                 "[MS-GC] FreeManager segmentRegionCount must not be zero, segmentSize=%zu regionSize=%zu",
                 segmentSize_, GetRegionSizeBytes());
    totalHotFreeRegions_.store(0, std::memory_order_relaxed);
    availabilityGeneration_.store(0, std::memory_order_relaxed);
    scavengeEpoch_.store(1, std::memory_order_relaxed);
    shards_.clear();
    for (uint32_t shardIdx = 0; shardIdx < shardCount_; ++shardIdx) {
        shards_.emplace_back();
        shards_.back().shardId_ = static_cast<uint16_t>(shardIdx);
    }
    nonEmptyShardWordCount_ = (static_cast<size_t>(shardCount_) + kChunkBits - 1) / kChunkBits;
    nonEmptyShardWords_.reset(new std::atomic<uint64_t>[nonEmptyShardWordCount_]);
    for (size_t wordIdx = 0; wordIdx < nonEmptyShardWordCount_; ++wordIdx) {
        nonEmptyShardWords_[wordIdx].store(0, std::memory_order_relaxed);
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
        shardState.scavengingRegionCount_ = 0;
        for (OwnedSegmentView& view : shardState.ownedViews_) {
            for (size_t chunkIdx = 0; chunkIdx < view.chunkSummaries_.size(); ++chunkIdx) {
                UpdateChunkSummaryLocked(view, chunkIdx);
            }
        }
    }
    for (uint32_t shardIdx = 0; shardIdx < shardCount_; ++shardIdx) {
        FreeManagerShard& shardState = GetShard(static_cast<uint16_t>(shardIdx));
        RefreshRegionSnapshotsLocked(shardState);
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
        found = TryTakeLocalHotRegionsLocked(shardState, num, idx);
    }

    if (!found && num == 1) {
        found = TryStealSingleRegionBatchLocked(homeShardId, idx, reusedSpan, true);
    } else if (!found && num > 1) {
        found = TryStealContiguousRegionRunLocked(homeShardId, num, idx, reusedSpan, true);
    }

    // Only after every shard has been probed for hot-free supply may this
    // allocation reuse pages previously returned to the OS.
    if (!found) {
        FreeManagerShard& shardState = GetShard(homeShardId);
        std::lock_guard<std::mutex> shardLock(shardState.shardMutex_);
        found = TryTakeLocalRegionsLocked(shardState, num, idx, reusedScavenged);
    }
    if (!found && num == 1) {
        found = TryStealSingleRegionBatchLocked(homeShardId, idx, reusedSpan, false);
    } else if (!found && num > 1) {
        found = TryStealContiguousRegionRunLocked(homeShardId, num, idx, reusedSpan, false);
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

bool FreeManager::TryTakeLocalHotRegionsLocked(FreeManagerShard& shard,
                                                RegionCount num,
                                                RegionIndex& idx)
{
    bool fromTakeSide = false;
    if (!FindOwnedRunLocked(shard, num, idx, fromTakeSide, true)) {
        return false;
    }

    bool reusedScavenged = false;
    MarkAllocatedLocked(shard, idx, num, reusedScavenged);
    CHECK_DETAIL(!reusedScavenged,
                 "[MS-GC] hot-only Region selection reused scavenged pages");
    if (fromTakeSide && idx == shard.takeCursor_) {
        shard.takeCursor_ = FindNextOwnedFreeRegionLocked(shard, idx + num);
    }
    if (!fromTakeSide && idx == shard.reclaimCursor_) {
        shard.reclaimCursor_ = FindNextOwnedFreeRegionLocked(shard, idx + num);
    }
    if (num == 1) {
        OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shard, idx);
        CHECK_DETAIL(view != nullptr,
                     "[MS-GC] hot single Region view missing: region=%u", idx);
        shard.fastTakeHintValid_ = true;
        for (size_t viewIdx = 0; viewIdx < shard.ownedViews_.size(); ++viewIdx) {
            if (&shard.ownedViews_[viewIdx] == view) {
                shard.fastTakeViewIdx_ = viewIdx;
                break;
            }
        }
        shard.fastTakeWordOffset_ =
            static_cast<size_t>(idx - view->regionStart_) / kChunkBits;
    }
    return true;
}

bool FreeManager::TryTakeLocalRegionsLocked(FreeManagerShard& shard,
                                            RegionCount num,
                                            RegionIndex& idx,
                                            bool& reusedScavenged)
{
    if (num == 1 && TryTakeOneRegionFastLocked(shard, idx, reusedScavenged)) {
        return true;
    }

    bool fromTakeSide = false;
    if (!FindPreferredOwnedFreeRunLocked(shard, num, idx, fromTakeSide)) {
        return false;
    }
    bool fromReclaimSide = !fromTakeSide;

    MarkAllocatedLocked(shard, idx, num, reusedScavenged);
    if (fromTakeSide && idx == shard.takeCursor_) {
        shard.takeCursor_ = FindNextOwnedFreeRegionLocked(shard, idx + num);
    }
    if (fromReclaimSide && idx == shard.reclaimCursor_) {
        shard.reclaimCursor_ = FindNextOwnedFreeRegionLocked(shard, idx + num);
    }
    return true;
}

void FreeManager::ReclaimRegionsReleased(RegionIndex idx, RegionCount num)
{
    if (num == 0) {
        return;
    }
    RegionSpan span { idx, num };
    ReclaimRegionsReleasedImpl(&span, 1);
}

void FreeManager::ReclaimRegionsReleasedBatch(const std::vector<RegionSpan>& spans)
{
    ReclaimRegionsReleasedImpl(spans.data(), spans.size());
}

void FreeManager::ReclaimRegionsReleasedImpl(const RegionSpan* spans, size_t count)
{
    if (count == 0) {
        return;
    }

    std::vector<std::vector<RegionSpan>> groupedSpans(shardCount_);
    {
        std::lock_guard<std::mutex> pageLock(pageHeapMutex_);
        for (size_t i = 0; i < count; ++i) {
            AppendGroupedOwnerSpansLocked(groupedSpans, spans[i].idx, spans[i].num);
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
    RegionIndex oldRegionCount = static_cast<RegionIndex>(committedRegionCount_);
    size_t newCommittedRegionCount = committedRegionCount_ + static_cast<size_t>(growRegions);
    size_t oldCommittedRegionCount = committedRegionCount_;
    size_t oldCommittedSegmentCount =
        (oldCommittedRegionCount + segmentRegionCount_ - 1) / segmentRegionCount_;
    size_t newCommittedSegmentCount =
        (newCommittedRegionCount + segmentRegionCount_ - 1) / segmentRegionCount_;
    ownerShardId = static_cast<uint16_t>(static_cast<uint32_t>(ownerShardId) % shardCount_);

    handler.InitializeFreshRegions(oldRegionCount, growRegions);

    FreeManagerShard& shardState = GetShard(ownerShardId);
    while (true) {
        // Grow still probes page metadata first, but if the owner shard is busy
        // we must not pin pageHeapMutex_ while waiting. Retry after yielding so
        // background shrink/feed helpers cannot block large-object growth.
        std::unique_lock<std::mutex> pageLock(pageHeapMutex_);
        std::unique_lock<std::mutex> shardLock(shardState.shardMutex_, std::try_to_lock);
        if (!shardLock.owns_lock()) {
            pageLock.unlock();
            std::this_thread::yield();
            continue;
        }
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
        PublishCommittedTopology(newCommittedRegionCount, oldCommittedEnd + growBytes);
        if (shardState.ownedViews_.size() == 1) {
            shardState.takeCursor_ = oldRegionCount;
        } else {
            shardState.takeCursor_ = std::min(shardState.takeCursor_, oldRegionCount);
        }
        shardState.reclaimCursor_ = std::min(shardState.reclaimCursor_,
                                             static_cast<RegionIndex>(committedRegionCount_));
        shardState.fastTakeHintValid_ = false;
        break;
    }

    handler.OnCommittedHeapExtended(oldCommittedEnd + growBytes);
    std::string shardSegments =
        DescribeCommittedShardSegmentsLocked(newCommittedSegmentCount);
    VLOG(REPORT,
         "[MS-GC] heap grow: condition=one-segment-demand requestRegions=%u ownerShard=%u "
         "segments=%zu->%zu shardSegments=%s committedRegions=%zu->%zu "
         "capacity=%.2fMB->%.2fMB",
         static_cast<unsigned>(growRegions), static_cast<unsigned>(ownerShardId),
         oldCommittedSegmentCount, newCommittedSegmentCount, shardSegments.c_str(),
         oldCommittedRegionCount, newCommittedRegionCount,
         MSBytesToMB(static_cast<size_t>(oldCommittedEnd - heapStart_)),
         MSBytesToMB(static_cast<size_t>(oldCommittedEnd + growBytes - heapStart_)));
    return true;
}

bool FreeManager::GrowAndTakeRegionsLocked(RegionCount numRegions,
                                           uint16_t ownerShardId,
                                           RegionIndex& idx,
                                           HeapRangeHandler& handler)
{
    uintptr_t oldCommittedEnd = committedEnd_.load(std::memory_order_acquire);
    if (numRegions == 0 || oldCommittedEnd >= maxHeapEnd_) {
        return false;
    }

    size_t regionSize = GetRegionSizeBytes();
    size_t remainingRegions = (maxHeapEnd_ - oldCommittedEnd) / regionSize;
    if (static_cast<size_t>(numRegions) > remainingRegions) {
        return false;
    }

    size_t growSegmentCount =
        (static_cast<size_t>(numRegions) + segmentRegionCount_ - 1) / segmentRegionCount_;
    size_t remainingBytes = maxHeapEnd_ - oldCommittedEnd;
    size_t growBytes = std::min(growSegmentCount * segmentSize_, remainingBytes);
    RegionCount growRegions = static_cast<RegionCount>(growBytes / regionSize);
    if (growRegions < numRegions) {
        return false;
    }

    RegionIndex oldRegionCount = static_cast<RegionIndex>(committedRegionCount_);
    CHECK_DETAIL(static_cast<size_t>(oldRegionCount) % segmentRegionCount_ == 0,
                 "[MS-GC] committed heap must be Segment-aligned before Grow-and-take: committedRegions=%u "
                 "segmentRegions=%zu",
                 oldRegionCount, segmentRegionCount_);
    size_t oldCommittedRegionCount = committedRegionCount_;
    size_t newCommittedRegionCount = oldCommittedRegionCount + static_cast<size_t>(growRegions);
    size_t oldCommittedSegmentCount =
        (oldCommittedRegionCount + segmentRegionCount_ - 1) / segmentRegionCount_;
    size_t newCommittedSegmentCount =
        (newCommittedRegionCount + segmentRegionCount_ - 1) / segmentRegionCount_;
    uintptr_t newCommittedEnd = oldCommittedEnd + growBytes;
    ownerShardId = static_cast<uint16_t>(static_cast<uint32_t>(ownerShardId) % shardCount_);

    handler.InitializeFreshRegions(oldRegionCount, growRegions);

    FreeManagerShard& shardState = GetShard(ownerShardId);
    while (true) {
        std::unique_lock<std::mutex> pageLock(pageHeapMutex_);
        std::unique_lock<std::mutex> shardLock(shardState.shardMutex_, std::try_to_lock);
        if (!shardLock.owns_lock()) {
            pageLock.unlock();
            std::this_thread::yield();
            continue;
        }

        size_t growStartSegmentIdx = oldCommittedRegionCount / segmentRegionCount_;
        size_t growEndSegmentIdx =
            (newCommittedRegionCount + segmentRegionCount_ - 1) / segmentRegionCount_;
        for (size_t segmentIdx = growStartSegmentIdx; segmentIdx < growEndSegmentIdx; ++segmentIdx) {
            CHECK_DETAIL(segmentIdx < segmentOwnerShard_.size(),
                         "[MS-GC] grown Segment exceeds owner table: segment=%zu ownerTableSize=%zu",
                         segmentIdx, segmentOwnerShard_.size());
            segmentOwnerShard_[segmentIdx] = ownerShardId;
            size_t segmentStartRegion = segmentIdx * segmentRegionCount_;
            RegionCount ownedRegionCount = static_cast<RegionCount>(
                std::min(newCommittedRegionCount - segmentStartRegion, segmentRegionCount_));
            AttachOwnedSegmentLocked(shardState, segmentIdx, ownedRegionCount);
            OwnedSegmentView* view = FindOwnedSegmentViewBySegmentIdxLocked(shardState, segmentIdx);
            CHECK_DETAIL(view != nullptr, "[MS-GC] grown Segment view missing: segment=%zu", segmentIdx);
            for (size_t chunkIdx = 0; chunkIdx < view->chunkSummaries_.size(); ++chunkIdx) {
                UpdateChunkSummaryLocked(*view, chunkIdx);
            }
        }

        PublishCommittedTopology(newCommittedRegionCount, newCommittedEnd);
        bool reusedScavenged = false;
        MarkAllocatedLocked(shardState, oldRegionCount, numRegions, reusedScavenged);
        CHECK_DETAIL(!reusedScavenged, "[MS-GC] freshly grown Regions must not be scavenged");
        idx = oldRegionCount;
        if (shardState.takeCursor_ >= oldRegionCount) {
            shardState.takeCursor_ =
                FindNextOwnedFreeRegionLocked(shardState, static_cast<RegionIndex>(oldRegionCount + numRegions));
        }
        shardState.reclaimCursor_ = std::min(
            shardState.reclaimCursor_, static_cast<RegionIndex>(committedRegionCount_));
        shardState.fastTakeHintValid_ = false;
        break;
    }

    handler.OnCommittedHeapExtended(newCommittedEnd);
    std::string shardSegments =
        DescribeCommittedShardSegmentsLocked(newCommittedSegmentCount);
    VLOG(REPORT,
         "[MS-GC] heap grow: condition=stable-no-free-supply requestRegions=%u ownerShard=%u "
         "segments=%zu->%zu shardSegments=%s committedRegions=%zu->%zu "
         "capacity=%.2fMB->%.2fMB",
         static_cast<unsigned>(numRegions), static_cast<unsigned>(ownerShardId),
         oldCommittedSegmentCount, newCommittedSegmentCount, shardSegments.c_str(),
         oldCommittedRegionCount, newCommittedRegionCount,
         MSBytesToMB(static_cast<size_t>(oldCommittedEnd - heapStart_)),
         MSBytesToMB(static_cast<size_t>(newCommittedEnd - heapStart_)));
    return true;
}

bool FreeManager::GrowOneSegment(uint16_t ownerShardId, HeapRangeHandler& handler)
{
    std::lock_guard<std::mutex> growLock(growMutex_);
    return GrowOneSegmentLocked(ownerShardId, handler);
}

bool FreeManager::TryTakeOrGrowRegions(RegionCount num,
                                       uint16_t homeShardId,
                                       RegionIndex& idx,
                                       HeapRangeHandler& handler)
{
    if (num == 0) {
        return false;
    }

    std::lock_guard<std::mutex> growLock(growMutex_);
    for (;;) {
        uint64_t generationBeforeTake = availabilityGeneration_.load(std::memory_order_acquire);
        if (TryTakeRegions(num, homeShardId, idx) == TryTakeResult::SUCCESS) {
            return true;
        }
        uint64_t generationAfterTake = availabilityGeneration_.load(std::memory_order_acquire);
        if (generationAfterTake == generationBeforeTake) {
            // Linearization point for the Grow decision: no supply was
            // published during this complete scan. A reclaim published after
            // this acquire is ordered after the decision and may coexist with
            // the newly grown supply; reclaim must not take growMutex_ because
            // its shard->grow lock order would invert Grow's grow->shard order.
            break;
        }
        // Reclaim published supply while the complete shard scan was in
        // progress. Repeat until one scan observes a stable generation.
    }
    return GrowAndTakeRegionsLocked(num, homeShardId, idx, handler);
}

size_t FreeManager::ScavengeFreeRegionsByRegionBudget(size_t regionBudget)
{
    if (regionBudget == 0 || committedRegionCount_ == 0) {
        return 0;
    }

    size_t scavengedRegions = 0;
    uint16_t startShardId = nextScavengeShard_;
    for (uint32_t scannedShardCount = 0;
         scannedShardCount < shardCount_ && scavengedRegions < regionBudget;
         ++scannedShardCount) {
        uint16_t ownerShardId = static_cast<uint16_t>((startShardId + scannedShardCount) % shardCount_);
        FreeManagerShard& shardState = GetShard(ownerShardId);
        std::vector<PendingScavengeSpan> spans;
        {
            std::unique_lock<std::mutex> shardLock(shardState.shardMutex_, std::try_to_lock);
            if (!shardLock.owns_lock()) {
                continue;
            }
            (void)ClaimScavengeSpansLocked(
                shardState, regionBudget - scavengedRegions, spans);
        }

        size_t releasedRegions = 0;
        for (PendingScavengeSpan& span : spans) {
            span.released = ReleaseUnits(RegionToUnitIdx(span.idx), RegionCountToUnitCount(span.num));
            if (span.released) {
                releasedRegions += span.num;
            }
        }

        if (!spans.empty()) {
            std::lock_guard<std::mutex> shardLock(shardState.shardMutex_);
            for (const PendingScavengeSpan& span : spans) {
                CompleteScavengingLocked(shardState, span.idx, span.num, span.released);
            }
        }
        scavengedRegions += releasedRegions;
    }
    nextScavengeShard_ = static_cast<uint16_t>((static_cast<uint32_t>(startShardId) + 1) % shardCount_);

    return scavengedRegions;
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

size_t FreeManager::GetOwnedSegmentCount(uint16_t ownerShardId) const
{
    const FreeManagerShard& shardState = GetShard(ownerShardId);
    std::lock_guard<std::mutex> lg(shardState.shardMutex_);
    return shardState.ownedViews_.size();
}

size_t FreeManager::GetHotFreeBytes() const
{
    return totalHotFreeRegions_.load(std::memory_order_relaxed) * GetRegionSizeBytes();
}

FreeManager::SegmentFreeSnapshot FreeManager::GetSegmentFreeSnapshot(size_t segmentIdx) const
{
    SegmentFreeSnapshot snapshot;
    if (segmentRegionCount_ == 0) {
        return snapshot;
    }

    size_t segmentStart = segmentIdx * segmentRegionCount_;
    size_t committedRegionCountSnapshot = 0;
    uint16_t ownerShardId = 0;
    {
        std::lock_guard<std::mutex> pageLock(pageHeapMutex_);
        if (segmentStart >= committedRegionCount_ || segmentIdx >= segmentOwnerShard_.size()) {
            return snapshot;
        }
        committedRegionCountSnapshot = committedRegionCount_;
        ownerShardId = segmentOwnerShard_[segmentIdx];
    }

    const FreeManagerShard& shardState = GetShard(ownerShardId);
    std::unique_lock<std::mutex> shardLock(shardState.shardMutex_, std::try_to_lock);
    if (!shardLock.owns_lock()) {
        return snapshot;
    }
    const OwnedSegmentView* view = FindOwnedSegmentViewBySegmentIdxLocked(shardState, segmentIdx);
    if (view == nullptr) {
        return snapshot;
    }

    if (static_cast<size_t>(view->regionStart_) >= committedRegionCountSnapshot) {
        return snapshot;
    }

    snapshot.validRegions = std::min(
        committedRegionCountSnapshot - static_cast<size_t>(view->regionStart_),
        segmentRegionCount_);
    if (snapshot.validRegions == 0) {
        return snapshot;
    }

    snapshot.freeRegions = CountFreeInViewRangeLocked(
        *view, 0, static_cast<RegionCount>(snapshot.validRegions));
    snapshot.scavengedRegions = CountScavengedInViewRangeLocked(
        *view, 0, static_cast<RegionCount>(snapshot.validRegions));
    return snapshot;
}

size_t FreeManager::ReleaseCompletelyFreeSegment(size_t segmentIdx)
{
    if (segmentRegionCount_ == 0) {
        return 0;
    }

    size_t segmentStart = segmentIdx * segmentRegionCount_;
    size_t committedRegionCountSnapshot = 0;
    uint16_t ownerShardId = 0;
    {
        std::lock_guard<std::mutex> pageLock(pageHeapMutex_);
        if (segmentStart >= committedRegionCount_ || segmentIdx >= segmentOwnerShard_.size()) {
            return 0;
        }
        committedRegionCountSnapshot = committedRegionCount_;
        ownerShardId = segmentOwnerShard_[segmentIdx];
    }

    FreeManagerShard& shardState = GetShard(ownerShardId);
    std::unique_lock<std::mutex> shardLock(shardState.shardMutex_, std::try_to_lock);
    if (!shardLock.owns_lock()) {
        return 0;
    }
    OwnedSegmentView* view = FindOwnedSegmentViewBySegmentIdxLocked(shardState, segmentIdx);
    if (view == nullptr) {
        return 0;
    }

    if (static_cast<size_t>(view->regionStart_) >= committedRegionCountSnapshot) {
        return 0;
    }

    size_t validRegions = std::min(
        committedRegionCountSnapshot - static_cast<size_t>(view->regionStart_),
        segmentRegionCount_);
    if (validRegions == 0) {
        return 0;
    }

    size_t freeRegions = CountFreeInViewRangeLocked(*view, 0, static_cast<RegionCount>(validRegions));
    if (freeRegions != validRegions) {
        return 0;
    }

    size_t scavengedRegions = CountScavengedInViewRangeLocked(*view, 0, static_cast<RegionCount>(validRegions));
    size_t hotFreeRegions = freeRegions > scavengedRegions ? (freeRegions - scavengedRegions) : 0;
    if (hotFreeRegions == 0) {
        return 0;
    }

    std::vector<PendingScavengeSpan> spans;
    size_t claimedRegions = ClaimScavengeSpansInViewLocked(
        shardState, *view, validRegions, hotFreeRegions, spans);
    CHECK_DETAIL(claimedRegions == hotFreeRegions,
                 "[MS-GC] failed to claim all hot-free Regions in free segment: segment=%zu expected=%zu actual=%zu",
                 segmentIdx, hotFreeRegions, claimedRegions);
    shardLock.unlock();

    size_t releasedRegions = 0;
    for (PendingScavengeSpan& span : spans) {
        span.released = ReleaseUnits(RegionToUnitIdx(span.idx), RegionCountToUnitCount(span.num));
        if (span.released) {
            releasedRegions += span.num;
        }
    }

    shardLock.lock();
    for (const PendingScavengeSpan& span : spans) {
        CompleteScavengingLocked(shardState, span.idx, span.num, span.released);
    }
    return releasedRegions * GetRegionSizeBytes();
}

size_t FreeManager::ScavengeFreeRegionsUpTo(size_t maxBytes)
{
    if (maxBytes == 0) {
        return 0;
    }
    size_t regionSize = GetRegionSizeBytes();
    size_t regionBudget = (maxBytes + regionSize - 1) / regionSize;
    return ScavengeFreeRegionsByRegionBudget(regionBudget) * regionSize;
}

uint64_t FreeManager::AgedHotFreeMaskForWord(
    const OwnedSegmentView& view, size_t wordIdx, size_t committedLocal,
    uint32_t currentEpoch, uint32_t minimumAge) const
{
    size_t wordBase = wordIdx * kChunkBits;
    uint64_t hot = HotFreeMaskForWord(view, wordIdx, committedLocal);
    uint64_t eligible = 0;
    while (hot != 0) {
        size_t bit = static_cast<size_t>(__builtin_ctzll(hot));
        size_t localIdx = wordBase + bit;
        uint32_t freeEpoch = view.freeEpochs_[localIdx];
        if (freeEpoch != kNoFreeEpoch &&
            HasMSScavengeEpochAge(currentEpoch, freeEpoch, minimumAge)) {
            eligible |= 1ULL << bit;
        }
        hot &= hot - 1;
    }
    return eligible;
}

void FreeManager::AdvanceScavengeEpoch(uint32_t epoch)
{
    // Per-Region freeEpoch is authoritative. Publishing the new epoch is
    // sufficient; Scavenge validates each candidate again under its shard lock.
    scavengeEpoch_.store(epoch, std::memory_order_release);
}

size_t FreeManager::ClaimAgedScavengeSpansLocked(
    FreeManagerShard& shard, size_t regionBudget, uint32_t currentEpoch,
    uint32_t minimumAge,
    std::vector<PendingScavengeSpan>& spans)
{
    size_t claimed = 0;
    for (OwnedSegmentView& view : shard.ownedViews_) {
        size_t committedLocal = view.regionStart_ < committedRegionCount_
            ? static_cast<size_t>(std::min(
                  view.regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) -
              view.regionStart_)
            : 0;
        for (size_t wordIdx = 0;
             wordIdx < view.bitmapWordCount_ && claimed < regionBudget;
             ++wordIdx) {
            uint64_t aged = AgedHotFreeMaskForWord(
                view, wordIdx, committedLocal, currentEpoch, minimumAge);
            size_t remaining = regionBudget - claimed;
            uint64_t claimMask = 0;
            while (aged != 0 &&
                   static_cast<size_t>(__builtin_popcountll(claimMask)) < remaining) {
                size_t bit = static_cast<size_t>(__builtin_ctzll(aged));
                claimMask |= 1ULL << bit;
                aged &= aged - 1;
            }
            claimed += ClaimScavengeWordMaskLocked(
                shard, view, wordIdx, claimMask, spans);
        }
        if (claimed >= regionBudget) {
            break;
        }
    }
    return claimed;
}

size_t FreeManager::ScavengeAgedRegionsUpTo(size_t maxBytes)
{
    if (maxBytes == 0 || committedRegionCount_ == 0) {
        return 0;
    }

    size_t regionSize = GetRegionSizeBytes();
    size_t regionBudget = maxBytes / regionSize;
    if (regionBudget == 0) {
        return 0;
    }
    uint32_t currentEpoch = scavengeEpoch_.load(std::memory_order_acquire);
    uint16_t startShardId = nextScavengeShard_;
    std::vector<std::vector<PendingScavengeSpan>> staged(shardCount_);
    size_t claimedRegions = 0;
    // Prefer the oldest hot-free Regions without collecting or sorting a
    // candidate list: scan all shards for old age buckets first, then fall
    // back to younger eligible buckets only when the release budget remains.
    static constexpr uint32_t kAgeFloors[] = {4, 2, 1};
    for (uint32_t minimumAge : kAgeFloors) {
        for (uint32_t scannedShardCount = 0;
             scannedShardCount < shardCount_ && claimedRegions < regionBudget;
             ++scannedShardCount) {
            uint16_t ownerShardId = static_cast<uint16_t>(
                (startShardId + scannedShardCount) % shardCount_);
            FreeManagerShard& shard = GetShard(ownerShardId);
            std::unique_lock<std::mutex> shardLock(
                shard.shardMutex_, std::try_to_lock);
            if (!shardLock.owns_lock()) {
                continue;
            }
            claimedRegions += ClaimAgedScavengeSpansLocked(
                shard, regionBudget - claimedRegions, currentEpoch, minimumAge,
                staged[ownerShardId]);
        }
    }

    if (claimedRegions == 0) {
        nextScavengeShard_ = static_cast<uint16_t>(
            (static_cast<uint32_t>(startShardId) + 1) % shardCount_);
        return 0;
    }

    size_t releasedRegions = 0;
    for (uint16_t shardId = 0; shardId < shardCount_; ++shardId) {
        for (PendingScavengeSpan& span : staged[shardId]) {
            span.released = ReleaseUnits(
                RegionToUnitIdx(span.idx), RegionCountToUnitCount(span.num));
            if (span.released) {
                releasedRegions += span.num;
            }
        }
        if (!staged[shardId].empty()) {
            FreeManagerShard& shard = GetShard(shardId);
            std::lock_guard<std::mutex> shardLock(shard.shardMutex_);
            for (const PendingScavengeSpan& span : staged[shardId]) {
                CompleteScavengingLocked(shard, span.idx, span.num, span.released);
            }
        }
    }
    nextScavengeShard_ = static_cast<uint16_t>(
        (static_cast<uint32_t>(startShardId) + 1) % shardCount_);
    return releasedRegions * regionSize;
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

FreeManager::RegionIndex FreeManager::FindFirstHotFreeRegionInViewLocked(
    const OwnedSegmentView& view, RegionIndex start) const
{
    RegionIndex viewEnd = std::min(
        view.regionEnd_, static_cast<RegionIndex>(committedRegionCount_));
    RegionIndex cursor = std::max(start, view.regionStart_);
    if (cursor >= viewEnd) {
        return static_cast<RegionIndex>(committedRegionCount_);
    }

    size_t committedLocal = static_cast<size_t>(viewEnd - view.regionStart_);
    size_t localStart = static_cast<size_t>(cursor - view.regionStart_);
    for (size_t wordIdx = localStart / kChunkBits;
         wordIdx < view.bitmapWordCount_; ++wordIdx) {
        uint64_t hotMask = HotFreeMaskForWord(view, wordIdx, committedLocal);
        size_t wordBase = wordIdx * kChunkBits;
        if (localStart > wordBase) {
            size_t skipBits = localStart - wordBase;
            if (skipBits >= kChunkBits) {
                continue;
            }
            hotMask &= ~((1ULL << skipBits) - 1);
        }
        if (hotMask != 0) {
            return static_cast<RegionIndex>(
                view.regionStart_ + wordBase +
                static_cast<size_t>(__builtin_ctzll(hotMask)));
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
        // Chunk-summary skip is only valid when the requested run fits within a
        // single summary chunk. For larger runs, a view can satisfy the request
        // via a cross-chunk free span even when every per-chunk max is smaller.
        if (num <= kChunkBits && probe % kChunkBits == 0 && !view.chunkSummaries_.empty()) {
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
    while (!shard.borrowedSpans_.empty()) {
        BorrowedSpan& span = shard.borrowedSpans_.front();
        if (span.num == 0) {
            shard.borrowedSpans_.pop_front();
            continue;
        }
        idx = span.idx;
        ++span.idx;
        --span.num;
        --shard.borrowedRegionCount_;
        if (span.num == 0) {
            shard.borrowedSpans_.pop_front();
        }
        return true;
    }
    return false;
}

bool FreeManager::TryTakeOneRegionFastLocked(FreeManagerShard& shard, RegionIndex& idx, bool& reusedScavenged)
{
    if (shard.ownedViews_.empty()) {
        return false;
    }
    bool fromTakeSide = false;
    if (!FindPreferredOwnedFreeRunLocked(shard, 1, idx, fromTakeSide)) {
        shard.fastTakeHintValid_ = false;
        return false;
    }

    OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shard, idx);
    CHECK_DETAIL(view != nullptr, "[MS-GC] preferred single Region view missing: region=%u", idx);
    MarkAllocatedOneLocked(
        shard, *view, static_cast<size_t>(idx - view->regionStart_), reusedScavenged);
    if (fromTakeSide && idx == shard.takeCursor_) {
        shard.takeCursor_ = FindNextOwnedFreeRegionLocked(shard, idx + 1);
    }
    if (!fromTakeSide && idx == shard.reclaimCursor_) {
        shard.reclaimCursor_ = FindNextOwnedFreeRegionLocked(shard, idx + 1);
    }

    shard.fastTakeHintValid_ = true;
    for (size_t viewIdx = 0; viewIdx < shard.ownedViews_.size(); ++viewIdx) {
        if (&shard.ownedViews_[viewIdx] == view) {
            shard.fastTakeViewIdx_ = viewIdx;
            break;
        }
    }
    shard.fastTakeWordOffset_ = static_cast<size_t>(idx - view->regionStart_) / kChunkBits;
    return true;
}

void FreeManager::MarkAllocatedOneLocked(FreeManagerShard& shard,
                                         OwnedSegmentView& view,
                                         size_t localIdx,
                                         bool& reusedScavenged)
{
    CHECK_DETAIL(!TestBit(view.scavengingBits_.data(), localIdx),
                 "[MS-GC] cannot allocate Region while OS release is in progress: segment=%zu localIdx=%zu",
                 view.segmentIdx_, localIdx);
    bool wasScavenged = TestBit(view.scavengedBits_.data(), localIdx);
    bool wasFree = !TestBit(view.allocBits_.data(), localIdx);
    CHECK_DETAIL(wasFree,
                 "[MS-GC] MarkAllocatedOneLocked: region already allocated, segment=%zu localIdx=%zu",
                 view.segmentIdx_,
                 localIdx);
    view.freeEpochs_[localIdx] = kNoFreeEpoch;
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
    size_t committedLocal = static_cast<size_t>(
        std::min(view.regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - view.regionStart_);
    RefreshHotWordsInRangeLocked(view, localIdx, 1, committedLocal);
    RefreshRegionSnapshotsLocked(shard);
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

bool FreeManager::FindOwnedFreeRunLocked(const FreeManagerShard& shard,
                                         RegionIndex cursor,
                                         RegionCount num,
                                         RegionIndex& idx) const
{
    if (num == 0 || num > committedRegionCount_ || shard.freeRegionCount_ < num) {
        return false;
    }
    if (shard.ownedViews_.empty()) {
        return false;
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

bool FreeManager::IsHotFreeRunInOwnedViewsLocked(const FreeManagerShard& shard,
                                                  RegionIndex start,
                                                  RegionCount num) const
{
    if (num == 0 || static_cast<size_t>(start) + num > committedRegionCount_) {
        return false;
    }
    RegionIndex cursor = start;
    RegionCount remaining = num;
    while (remaining != 0) {
        const OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shard, cursor);
        if (view == nullptr) {
            return false;
        }
        RegionCount chunk = std::min<RegionCount>(
            remaining, static_cast<RegionCount>(view->regionEnd_ - cursor));
        size_t localStart = static_cast<size_t>(cursor - view->regionStart_);
        for (size_t localIdx = localStart; localIdx < localStart + chunk; ++localIdx) {
            if (TestBit(view->allocBits_.data(), localIdx) ||
                TestBit(view->scavengedBits_.data(), localIdx) ||
                TestBit(view->scavengingBits_.data(), localIdx)) {
                return false;
            }
        }
        cursor = static_cast<RegionIndex>(cursor + chunk);
        remaining = static_cast<RegionCount>(remaining - chunk);
    }
    return true;
}

bool FreeManager::FindOwnedHotFreeRunLocked(const FreeManagerShard& shard,
                                             RegionIndex cursor,
                                             RegionCount num,
                                             RegionIndex& idx) const
{
    if (num == 0 || shard.ownedViews_.empty() || shard.freeRegionCount_ < num) {
        return false;
    }
    const auto& views = shard.ownedViews_;
    auto it = std::lower_bound(views.begin(), views.end(), cursor,
        [](const OwnedSegmentView& view, RegionIndex value) {
            return view.regionEnd_ <= value;
        });
    for (; it != views.end(); ++it) {
        RegionIndex probe = std::max(cursor, it->regionStart_);
        RegionIndex viewEnd = std::min(
            it->regionEnd_, static_cast<RegionIndex>(committedRegionCount_));
        while (probe < viewEnd) {
            RegionIndex candidate = FindFirstHotFreeRegionInViewLocked(*it, probe);
            if (candidate >= viewEnd) {
                break;
            }
            if (static_cast<size_t>(candidate) + num <= committedRegionCount_ &&
                IsHotFreeRunInOwnedViewsLocked(shard, candidate, num)) {
                idx = candidate;
                return true;
            }
            probe = static_cast<RegionIndex>(candidate + 1);
        }
    }
    return false;
}

bool FreeManager::FindPreferredOwnedFreeRunLocked(const FreeManagerShard& shard,
                                                   RegionCount num,
                                                   RegionIndex& idx,
                                                   bool& fromTakeSide) const
{
    if (FindOwnedHotFreeRunLocked(shard, shard.takeCursor_, num, idx)) {
        fromTakeSide = true;
        return true;
    }
    if (shard.reclaimCursor_ < shard.takeCursor_ &&
        FindOwnedHotFreeRunLocked(shard, shard.reclaimCursor_, num, idx)) {
        fromTakeSide = false;
        return true;
    }
    if (FindOwnedFreeRunLocked(shard, shard.takeCursor_, num, idx)) {
        fromTakeSide = true;
        return true;
    }
    if (shard.reclaimCursor_ < shard.takeCursor_ &&
        FindOwnedFreeRunLocked(shard, shard.reclaimCursor_, num, idx)) {
        fromTakeSide = false;
        return true;
    }
    return false;
}

bool FreeManager::FindOwnedRunLocked(const FreeManagerShard& shard,
                                     RegionCount num,
                                     RegionIndex& idx,
                                     bool& fromTakeSide,
                                     bool hotOnly) const
{
    if (!hotOnly) {
        return FindPreferredOwnedFreeRunLocked(shard, num, idx, fromTakeSide);
    }
    if (FindOwnedHotFreeRunLocked(shard, shard.takeCursor_, num, idx)) {
        fromTakeSide = true;
        return true;
    }
    if (shard.reclaimCursor_ < shard.takeCursor_ &&
        FindOwnedHotFreeRunLocked(shard, shard.reclaimCursor_, num, idx)) {
        fromTakeSide = false;
        return true;
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

FreeManager::RegionCount FreeManager::CountContiguousHotFreeInViewLocked(
    const OwnedSegmentView& view, RegionIndex start, RegionCount maxCount)
{
    if (maxCount == 0 || start < view.regionStart_ || start >= view.regionEnd_) {
        return 0;
    }
    size_t localIdx = static_cast<size_t>(start - view.regionStart_);
    size_t localEnd = std::min(
        static_cast<size_t>(view.regionEnd_ - view.regionStart_),
        localIdx + static_cast<size_t>(maxCount));
    size_t begin = localIdx;
    while (localIdx < localEnd &&
           !TestBit(view.allocBits_.data(), localIdx) &&
           !TestBit(view.scavengedBits_.data(), localIdx) &&
           !TestBit(view.scavengingBits_.data(), localIdx)) {
        ++localIdx;
    }
    return static_cast<RegionCount>(localIdx - begin);
}

size_t FreeManager::ClaimScavengeWordMaskLocked(FreeManagerShard& shard,
                                                OwnedSegmentView& view,
                                                size_t wordIdx,
                                                uint64_t claimMask,
                                                std::vector<PendingScavengeSpan>& spans)
{
    if (claimMask == 0) {
        return 0;
    }

    size_t committedLocal = static_cast<size_t>(
        std::min(view.regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - view.regionStart_);
    uint64_t hotFree = HotFreeMaskForWord(view, wordIdx, committedLocal);
    CHECK_DETAIL((claimMask & ~hotFree) == 0,
                 "[MS-GC] word claim contains non-hot Region: segment=%zu word=%zu claim=0x%llx hot=0x%llx",
                 view.segmentIdx_, wordIdx,
                 static_cast<unsigned long long>(claimMask),
                 static_cast<unsigned long long>(hotFree));

    size_t claimedRegions = static_cast<size_t>(__builtin_popcountll(claimMask));
    CHECK_DETAIL(shard.freeRegionCount_ >= claimedRegions,
                 "[MS-GC] freeRegionCount underflow while claiming Scavenge word");
    view.allocBits_[wordIdx] |= claimMask;
    view.scavengingBits_[wordIdx] |= claimMask;
    shard.freeRegionCount_ -= claimedRegions;
    shard.scavengingRegionCount_ += claimedRegions;

    size_t wordBase = wordIdx * kChunkBits;
    UpdateChunkSummaryLocked(view, wordIdx);
    RefreshHotWordLocked(view, wordIdx, committedLocal);

    uint64_t pending = claimMask;
    while (pending != 0) {
        int hi = 63 - __builtin_clzll(pending);
        int lo = hi;
        while (lo > 0 && ((pending >> (lo - 1)) & 1ULL) != 0) {
            --lo;
        }
        size_t runLength = static_cast<size_t>(hi - lo + 1);
        RegionIndex runStart = static_cast<RegionIndex>(
            view.regionStart_ + wordBase + static_cast<size_t>(lo));
        uint64_t runMask = runLength == kChunkBits
            ? ~0ULL
            : (((1ULL << runLength) - 1) << lo);
        pending &= ~runMask;

        if (!spans.empty() &&
            static_cast<RegionIndex>(runStart + runLength) == spans.back().idx) {
            spans.back().idx = runStart;
            spans.back().num = static_cast<RegionCount>(spans.back().num + runLength);
        } else if (!spans.empty() &&
                   static_cast<RegionIndex>(spans.back().idx + spans.back().num) == runStart) {
            spans.back().num = static_cast<RegionCount>(spans.back().num + runLength);
        } else {
            spans.push_back({
                runStart,
                static_cast<RegionCount>(runLength),
                false
            });
        }
    }
    return claimedRegions;
}

size_t FreeManager::ClaimScavengeSpansLocked(FreeManagerShard& shard,
                                             size_t regionBudget,
                                             std::vector<PendingScavengeSpan>& spans)
{
    if (regionBudget == 0 || shard.freeRegionCount_ <= shard.scavengedRegionCount_ ||
        shard.ownedViews_.empty()) {
        return 0;
    }

    const size_t viewCount = shard.ownedViews_.size();
    size_t viewIdx = std::min(shard.scavengeViewCursor_, viewCount - 1);
    size_t wordCursor = shard.scavengeWordCursor_;
    size_t claimedRegions = 0;

    auto moveToPreviousView = [&](size_t currentView) {
        if (currentView == 0) {
            return viewCount - 1;
        }
        return currentView - 1;
    };

    for (size_t visitedViews = 0;
         visitedViews < viewCount && claimedRegions < regionBudget;
         ++visitedViews) {
        OwnedSegmentView& view = shard.ownedViews_[viewIdx];
        size_t committedLocal = 0;
        if (view.regionStart_ < committedRegionCount_) {
            committedLocal = static_cast<size_t>(
                std::min(view.regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) -
                view.regionStart_);
        }
        size_t wordCount = (committedLocal + kChunkBits - 1) / kChunkBits;
        uint64_t validWordMask = wordCount == 0
            ? 0
            : (wordCount == kChunkBits ? ~0ULL : ((1ULL << wordCount) - 1));
        uint64_t hotWords = view.hotWordMask_ & validWordMask;

        if (hotWords != 0) {
            size_t claimedBeforeView = claimedRegions;
            size_t currentWord = std::min(wordCursor, wordCount - 1);
            uint64_t remainingHotWords = hotWords;
            while (remainingHotWords != 0 && claimedRegions < regionBudget) {
                uint64_t atOrBelow = currentWord == kChunkBits - 1
                    ? remainingHotWords
                    : remainingHotWords & ((1ULL << (currentWord + 1)) - 1);
                if (atOrBelow == 0) {
                    atOrBelow = remainingHotWords;
                }
                size_t wordIdx = static_cast<size_t>(63 - __builtin_clzll(atOrBelow));
                remainingHotWords &= ~(1ULL << wordIdx);

                uint64_t hotFree = HotFreeMaskForWord(view, wordIdx, committedLocal);
                size_t remainingBudget = regionBudget - claimedRegions;
                uint64_t claimMask = 0;
                size_t selectedRegions = 0;
                while (hotFree != 0 && selectedRegions < remainingBudget) {
                    size_t bit = static_cast<size_t>(63 - __builtin_clzll(hotFree));
                    uint64_t bitMask = 1ULL << bit;
                    claimMask |= bitMask;
                    hotFree &= ~bitMask;
                    ++selectedRegions;
                }
                claimedRegions += ClaimScavengeWordMaskLocked(
                    shard, view, wordIdx, claimMask, spans);

                if (claimedRegions >= regionBudget) {
                    view.maxFreeRun_ = ComputeMaxFreeRunInViewLocked(view);
                    if (HotFreeMaskForWord(view, wordIdx, committedLocal) != 0) {
                        shard.scavengeViewCursor_ = viewIdx;
                        shard.scavengeWordCursor_ = wordIdx;
                    } else if (wordIdx != 0) {
                        shard.scavengeViewCursor_ = viewIdx;
                        shard.scavengeWordCursor_ = wordIdx - 1;
                    } else {
                        size_t nextView = moveToPreviousView(viewIdx);
                        shard.scavengeViewCursor_ = nextView;
                        size_t nextWordCount = shard.ownedViews_[nextView].bitmapWordCount_;
                        shard.scavengeWordCursor_ = nextWordCount == 0 ? 0 : nextWordCount - 1;
                    }
                    RefreshRegionSnapshotsLocked(shard);
                    return claimedRegions;
                }
                currentWord = wordIdx == 0 ? wordCount - 1 : wordIdx - 1;
            }
            if (claimedRegions != claimedBeforeView) {
                view.maxFreeRun_ = ComputeMaxFreeRunInViewLocked(view);
            }
        }

        viewIdx = moveToPreviousView(viewIdx);
        wordCursor = shard.ownedViews_[viewIdx].bitmapWordCount_ == 0
            ? 0
            : shard.ownedViews_[viewIdx].bitmapWordCount_ - 1;
        shard.scavengeViewCursor_ = viewIdx;
        shard.scavengeWordCursor_ = wordCursor;
    }

    if (claimedRegions != 0) {
        RefreshRegionSnapshotsLocked(shard);
    }
    return claimedRegions;
}

size_t FreeManager::ClaimScavengeSpansInViewLocked(FreeManagerShard& shard,
                                                   OwnedSegmentView& view,
                                                   size_t committedLocal,
                                                   size_t regionBudget,
                                                   std::vector<PendingScavengeSpan>& spans)
{
    if (regionBudget == 0 || committedLocal == 0) {
        return 0;
    }

    size_t wordCount = (committedLocal + kChunkBits - 1) / kChunkBits;
    size_t claimedRegions = 0;

    // Use the same state transition as budgeted and aged scavenging.  The
    // helper also coalesces adjacent word runs, so this retains the old
    // whole-segment release granularity without a second bitmap protocol.
    for (size_t wordIdx = wordCount;
         wordIdx-- > 0 && claimedRegions < regionBudget;) {
        uint64_t hotFree = HotFreeMaskForWord(view, wordIdx, committedLocal);
        size_t remainingBudget = regionBudget - claimedRegions;
        uint64_t claimMask = 0;
        while (hotFree != 0 && remainingBudget != 0) {
            size_t bit = static_cast<size_t>(63 - __builtin_clzll(hotFree));
            claimMask |= 1ULL << bit;
            hotFree &= ~(1ULL << bit);
            --remainingBudget;
        }
        claimedRegions += ClaimScavengeWordMaskLocked(
            shard, view, wordIdx, claimMask, spans);
    }
    if (claimedRegions != 0) {
        view.maxFreeRun_ = ComputeMaxFreeRunInViewLocked(view);
        RefreshRegionSnapshotsLocked(shard);
    }
    return claimedRegions;
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
            std::min(committedRegionCount_.load(std::memory_order_acquire),
                     (segmentIdx + 1) * segmentRegionCount_));
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
                                                  RegionSpan& reusedSpan,
                                                  bool hotOnly)
{
    if (receiverShardId >= shardCount_ || shardCount_ <= 1) {
        return false;
    }

    FreeManagerShard& receiverShard = GetShard(receiverShardId);
    uint32_t searchStart =
        static_cast<uint32_t>(receiverShard.nextStealDonorShard_.fetch_add(1, std::memory_order_relaxed)) %
        shardCount_;
    uint32_t remainingShardCount = shardCount_;

    while (remainingShardCount != 0) {
        uint16_t donorShardId = 0;
        if (!FindNextNonEmptyDonorShard(receiverShardId, searchStart, donorShardId)) {
            break;
        }
        uint32_t donorDistance = donorShardId >= searchStart
            ? static_cast<uint32_t>(donorShardId) - searchStart
            : shardCount_ - searchStart + static_cast<uint32_t>(donorShardId);
        if (donorDistance >= remainingShardCount) {
            break;
        }
        remainingShardCount -= donorDistance + 1;
        searchStart = (static_cast<uint32_t>(donorShardId) + 1) % shardCount_;

        RegionIndex donorIdx = 0;
        RegionCount batchCount = 0;
        bool batchReusedScavenged = false;
        {
            FreeManagerShard& donorShard = GetShard(donorShardId);
            std::lock_guard<std::mutex> donorLock(donorShard.shardMutex_);

            bool fromTakeSide = false;
            if (!FindOwnedRunLocked(
                    donorShard, 1, donorIdx, fromTakeSide, hotOnly)) {
                continue;
            }
            bool fromReclaimSide = !fromTakeSide;

            // Bitmap-based contiguous count via ctzll (O(1) for the steal batch).
            OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(donorShard, donorIdx);
            if (view == nullptr) {
                continue;
            }
            batchCount = CountContiguousHotFreeInViewLocked(
                *view, donorIdx, kSingleRegionStealBatch);
            if (batchCount == 0 && !hotOnly) {
                // Do not reserve a batch of scavenged Regions. Cold fallback
                // takes exactly the requested Region so later allocations can
                // re-check global hot supply before faulting pages back in.
                batchCount = CountContiguousFreeInViewLocked(
                    *view, donorIdx, 1);
            }
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
        if (batchCount > 1) {
            // Publish borrowed batches under the receiver lock so consume-side
            // reads cannot observe a torn (idx, num) pair, and multiple pending
            // steals queue instead of overwriting each other.
            std::lock_guard<std::mutex> receiverLock(receiverShard.shardMutex_);
            receiverShard.borrowedSpans_.push_back(
                BorrowedSpan { donorShardId,
                               static_cast<RegionIndex>(donorIdx + 1),
                               static_cast<RegionCount>(batchCount - 1) });
            receiverShard.borrowedRegionCount_ += static_cast<size_t>(batchCount - 1);
        }
        if (batchReusedScavenged) {
            reusedSpan.idx = donorIdx;
            reusedSpan.num = batchCount;
        }
        return true;
    }

    return false;
}

bool FreeManager::TryStealContiguousRegionRunLocked(uint16_t receiverShardId,
                                                    RegionCount num,
                                                    RegionIndex& idx,
                                                    RegionSpan& reusedSpan,
                                                    bool hotOnly)
{
    if (receiverShardId >= shardCount_ || shardCount_ <= 1 || num == 0) {
        return false;
    }

    FreeManagerShard& receiverShard = GetShard(receiverShardId);
    uint32_t searchStart =
        static_cast<uint32_t>(receiverShard.nextStealDonorShard_.fetch_add(1, std::memory_order_relaxed)) %
        shardCount_;
    uint32_t remainingShardCount = shardCount_;
    uint16_t bestDonorShardId = receiverShardId;
    size_t bestFreeRun = 0;
    size_t bestFreeRegions = 0;

    while (remainingShardCount != 0) {
        uint16_t donorShardId = 0;
        if (!FindNextNonEmptyDonorShard(receiverShardId, searchStart, donorShardId)) {
            break;
        }
        uint32_t donorDistance = donorShardId >= searchStart
            ? static_cast<uint32_t>(donorShardId) - searchStart
            : shardCount_ - searchStart + static_cast<uint32_t>(donorShardId);
        if (donorDistance >= remainingShardCount) {
            break;
        }
        remainingShardCount -= donorDistance + 1;
        searchStart = (static_cast<uint32_t>(donorShardId) + 1) % shardCount_;

        {
            const FreeManagerShard& donorShard = GetShard(donorShardId);
            std::lock_guard<std::mutex> donorLock(donorShard.shardMutex_);
            RegionIndex candidateIdx = 0;
            bool candidateFromTakeSide = false;
            if (!FindOwnedRunLocked(
                    donorShard, num, candidateIdx, candidateFromTakeSide,
                    hotOnly)) {
                continue;
            }
            const OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(donorShard, candidateIdx);
            if (view == nullptr) {
                continue;
            }
            size_t freeRun = CountContiguousFreeInViewLocked(
                *view, candidateIdx, static_cast<RegionCount>(view->regionEnd_ - candidateIdx));
            if (freeRun > bestFreeRun ||
                (freeRun == bestFreeRun && donorShard.freeRegionCount_ > bestFreeRegions)) {
                bestDonorShardId = donorShardId;
                bestFreeRun = freeRun;
                bestFreeRegions = donorShard.freeRegionCount_;
            }
        }
    }

    if (bestDonorShardId == receiverShardId) {
        return false;
    }

    FreeManagerShard& donorShard = GetShard(bestDonorShardId);
    RegionIndex donorIdx = 0;
    bool reusedScavenged = false;
    bool fromTakeSide = false;
    bool fromReclaimSide = false;
    {
        std::lock_guard<std::mutex> donorLock(donorShard.shardMutex_);
        if (!FindOwnedRunLocked(
                donorShard, num, donorIdx, fromTakeSide, hotOnly)) {
            return false;
        }
        fromReclaimSide = !fromTakeSide;

        MarkAllocatedLocked(donorShard, donorIdx, num, reusedScavenged);
        if (fromTakeSide && donorIdx == donorShard.takeCursor_) {
            donorShard.takeCursor_ = FindNextOwnedFreeRegionLocked(donorShard, donorIdx + num);
        }
        if (fromReclaimSide && donorIdx == donorShard.reclaimCursor_) {
            donorShard.reclaimCursor_ = FindNextOwnedFreeRegionLocked(donorShard, donorIdx + num);
        }
    }

    idx = donorIdx;
    if (reusedScavenged) {
        reusedSpan.idx = donorIdx;
        reusedSpan.num = num;
    }

    return true;
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
            CHECK_DETAIL(!TestBit(view->scavengingBits_.data(), localIdx),
                         "[MS-GC] cannot allocate Region while OS release is in progress: segment=%zu localIdx=%zu",
                         view->segmentIdx_, localIdx);
            CHECK_DETAIL(!TestBit(view->allocBits_.data(), localIdx),
                         "[MS-GC] MarkAllocatedLocked: region already allocated, segment=%zu localIdx=%zu",
                         view->segmentIdx_,
                         localIdx);
            SetBit(view->allocBits_.data(), localIdx);
            ClearBit(view->scavengedBits_.data(), localIdx);
            view->freeEpochs_[localIdx] = kNoFreeEpoch;
        }
        UpdateChunkSummaryRangeLocked(*view, localStart, chunk);  // also refreshes view.maxFreeRun_
        size_t committedLocal = static_cast<size_t>(
            std::min(view->regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - view->regionStart_);
        RefreshHotWordsInRangeLocked(*view, localStart, chunk, committedLocal);
        cursor = static_cast<RegionIndex>(cursor + chunk);
        remaining = static_cast<RegionCount>(remaining - chunk);
    }
    RefreshRegionSnapshotsLocked(shard);
}

void FreeManager::MarkFreeLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num)
{
    uint32_t currentEpoch = scavengeEpoch_.load(std::memory_order_relaxed);
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
            CHECK_DETAIL((view->scavengingBits_[wi] & mask) == 0,
                         "[MS-GC] cannot reclaim Region while OS release is in progress: segment=%zu word=%zu",
                         view->segmentIdx_, wi);

            shard.freeRegionCount_ += __builtin_popcountll(oldAlloc & mask);
            shard.scavengedRegionCount_ -= __builtin_popcountll(oldScav & mask);

            view->allocBits_[wi] = oldAlloc & ~mask;
            view->scavengedBits_[wi] = oldScav & ~mask;
        }
        for (size_t localIdx = localStart; localIdx < localEnd; ++localIdx) {
            view->freeEpochs_[localIdx] = currentEpoch;
        }
        UpdateChunkSummaryRangeLocked(*view, localStart, chunk);  // also refreshes view.maxFreeRun_
        size_t committedLocal = static_cast<size_t>(
            std::min(view->regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - view->regionStart_);
        RefreshHotWordsInRangeLocked(*view, localStart, chunk, committedLocal);
        cursor = static_cast<RegionIndex>(cursor + chunk);
        remaining = static_cast<RegionCount>(remaining - chunk);
    }
    RefreshRegionSnapshotsLocked(shard);
}

void FreeManager::MarkScavengingLocked(FreeManagerShard& shard, RegionIndex idx, RegionCount num)
{
    RegionIndex cursor = idx;
    RegionCount remaining = num;
    while (remaining != 0 && cursor < committedRegionCount_) {
        OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shard, cursor);
        CHECK_DETAIL(view != nullptr, "[MS-GC] scavenge claim target view missing: region=%u", cursor);
        RegionCount chunk = std::min<RegionCount>(
            remaining,
            static_cast<RegionCount>(std::min<RegionIndex>(view->regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - cursor));
        size_t localStart = static_cast<size_t>(cursor - view->regionStart_);
        for (size_t localIdx = localStart; localIdx < localStart + chunk; ++localIdx) {
            CHECK_DETAIL(!TestBit(view->allocBits_.data(), localIdx) &&
                         !TestBit(view->scavengedBits_.data(), localIdx) &&
                         !TestBit(view->scavengingBits_.data(), localIdx),
                         "[MS-GC] scavenge claim requires hot-free Region: segment=%zu localIdx=%zu",
                         view->segmentIdx_, localIdx);
            SetBit(view->allocBits_.data(), localIdx);
            SetBit(view->scavengingBits_.data(), localIdx);
        }
        CHECK_DETAIL(shard.freeRegionCount_ >= chunk,
                     "[MS-GC] freeRegionCount underflow while claiming scavenge span");
        shard.freeRegionCount_ -= chunk;
        shard.scavengingRegionCount_ += chunk;
        UpdateChunkSummaryRangeLocked(*view, localStart, chunk);
        size_t committedLocal = static_cast<size_t>(
            std::min(view->regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - view->regionStart_);
        RefreshHotWordsInRangeLocked(*view, localStart, chunk, committedLocal);
        cursor = static_cast<RegionIndex>(cursor + chunk);
        remaining = static_cast<RegionCount>(remaining - chunk);
    }
    CHECK_DETAIL(remaining == 0, "[MS-GC] scavenge claim extends past committed heap");
    RefreshRegionSnapshotsLocked(shard);
}

void FreeManager::CompleteScavengingLocked(FreeManagerShard& shard,
                                           RegionIndex idx,
                                           RegionCount num,
                                           bool released)
{
    RegionIndex cursor = idx;
    RegionCount remaining = num;
    while (remaining != 0 && cursor < committedRegionCount_) {
        OwnedSegmentView* view = FindOwnedSegmentViewByRegionLocked(shard, cursor);
        CHECK_DETAIL(view != nullptr, "[MS-GC] scavenge completion target view missing: region=%u", cursor);
        RegionCount chunk = std::min<RegionCount>(
            remaining,
            static_cast<RegionCount>(std::min<RegionIndex>(
                view->regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - cursor));
        size_t localStart = static_cast<size_t>(cursor - view->regionStart_);
        size_t localEnd = localStart + chunk;
        size_t firstWord = localStart / kChunkBits;
        size_t lastWord = (localEnd - 1) / kChunkBits;
        for (size_t wordIdx = firstWord; wordIdx <= lastWord; ++wordIdx) {
            size_t bitLo = wordIdx == firstWord ? localStart % kChunkBits : 0;
            size_t bitHi = wordIdx == lastWord ? ((localEnd - 1) % kChunkBits) + 1 : kChunkBits;
            uint64_t mask = bitHi == kChunkBits
                ? (~0ULL << bitLo)
                : (((1ULL << bitHi) - 1) & (~0ULL << bitLo));
            CHECK_DETAIL((view->allocBits_[wordIdx] & mask) == mask &&
                         (view->scavengingBits_[wordIdx] & mask) == mask &&
                         (view->scavengedBits_[wordIdx] & mask) == 0,
                         "[MS-GC] invalid Scavenge completion word state: segment=%zu word=%zu mask=0x%llx",
                         view->segmentIdx_, wordIdx, static_cast<unsigned long long>(mask));
            view->allocBits_[wordIdx] &= ~mask;
            view->scavengingBits_[wordIdx] &= ~mask;
            if (released) {
                view->scavengedBits_[wordIdx] |= mask;
            }
            UpdateChunkSummaryLocked(*view, wordIdx);
        }
        CHECK_DETAIL(shard.scavengingRegionCount_ >= chunk,
                     "[MS-GC] scavengingRegionCount underflow while completing scavenge span");
        shard.scavengingRegionCount_ -= chunk;
        shard.freeRegionCount_ += chunk;
        if (released) {
            shard.scavengedRegionCount_ += chunk;
            for (size_t localIdx = localStart; localIdx < localEnd; ++localIdx) {
                view->freeEpochs_[localIdx] = kNoFreeEpoch;
            }
        }
        view->maxFreeRun_ = ComputeMaxFreeRunInViewLocked(*view);
        size_t committedLocal = static_cast<size_t>(
            std::min(view->regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - view->regionStart_);
        RefreshHotWordsInRangeLocked(*view, localStart, chunk, committedLocal);
        cursor = static_cast<RegionIndex>(cursor + chunk);
        remaining = static_cast<RegionCount>(remaining - chunk);
    }
    CHECK_DETAIL(remaining == 0, "[MS-GC] scavenge completion extends past committed heap");
    RefreshRegionSnapshotsLocked(shard);
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
        for (size_t localIdx = localStart; localIdx < localStart + chunk; ++localIdx) {
            CHECK_DETAIL(!TestBit(view->scavengingBits_.data(), localIdx),
                         "[MS-GC] cannot make Region unavailable while OS release is in progress: segment=%zu localIdx=%zu",
                         view->segmentIdx_, localIdx);
        }
        shard.scavengedRegionCount_ -= CountScavengedInViewRangeLocked(*view, localStart, chunk);
        shard.freeRegionCount_ -= CountFreeInViewRangeLocked(*view, localStart, chunk);
        for (size_t localIdx = localStart; localIdx < localStart + chunk; ++localIdx) {
            SetBit(view->allocBits_.data(), localIdx);
            ClearBit(view->scavengedBits_.data(), localIdx);
            view->freeEpochs_[localIdx] = kNoFreeEpoch;
        }
        UpdateChunkSummaryRangeLocked(*view, localStart, chunk);  // also refreshes view.maxFreeRun_
        size_t committedLocal = static_cast<size_t>(
            std::min(view->regionEnd_, static_cast<RegionIndex>(committedRegionCount_)) - view->regionStart_);
        RefreshHotWordsInRangeLocked(*view, localStart, chunk, committedLocal);
        cursor = static_cast<RegionIndex>(cursor + chunk);
        remaining = static_cast<RegionCount>(remaining - chunk);
    }
    RefreshRegionSnapshotsLocked(shard);
}

void FreeManager::RefreshRegionSnapshotsLocked(FreeManagerShard& shard)
{
    if (shard.freeRegionCount_ > shard.publishedFreeRegionCount_) {
        availabilityGeneration_.fetch_add(1, std::memory_order_release);
    }
    shard.publishedFreeRegionCount_ = shard.freeRegionCount_;

    if (nonEmptyShardWords_ != nullptr) {
        size_t wordIdx = static_cast<size_t>(shard.shardId_) / kChunkBits;
        uint64_t shardMask = 1ULL << (shard.shardId_ % kChunkBits);
        bool nonEmpty = shard.freeRegionCount_ != 0;
        if (nonEmpty != shard.advertisedNonEmpty_) {
            if (nonEmpty) {
                nonEmptyShardWords_[wordIdx].fetch_or(shardMask, std::memory_order_release);
            } else {
                nonEmptyShardWords_[wordIdx].fetch_and(~shardMask, std::memory_order_release);
            }
            shard.advertisedNonEmpty_ = nonEmpty;
        }
    }

    uint32_t hotFree = static_cast<uint32_t>(
        shard.freeRegionCount_ >= shard.scavengedRegionCount_
            ? (shard.freeRegionCount_ - shard.scavengedRegionCount_) : 0);
    uint32_t oldHotFree = shard.snapHotFreeRegions_.exchange(hotFree, std::memory_order_relaxed);
    if (hotFree > oldHotFree) {
        totalHotFreeRegions_.fetch_add(static_cast<size_t>(hotFree - oldHotFree),
                                       std::memory_order_relaxed);
    } else if (oldHotFree > hotFree) {
        size_t delta = static_cast<size_t>(oldHotFree - hotFree);
        size_t oldTotal = totalHotFreeRegions_.fetch_sub(delta, std::memory_order_relaxed);
        CHECK_DETAIL(oldTotal >= delta,
                     "[MS-GC] totalHotFreeRegions underflow: old=%zu delta=%zu",
                     oldTotal, delta);
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

bool FreeManager::ReleaseUnits(UnitIndex idx, UnitCount num) const
{
    uintptr_t addr = heapStart_ + static_cast<uintptr_t>(idx) * unitSize_;
    size_t size = static_cast<size_t>(num) * unitSize_;
#if defined(_WIN64)
    if (UNLIKELY(!VirtualFree(reinterpret_cast<void*>(addr), size, MEM_DECOMMIT))) {
        LOG(RTLOG_ERROR, "[MS-GC] FreeManager::ReleaseUnits VirtualFree failed: addr=%p size=%zu",
            reinterpret_cast<void*>(addr), size);
        return false;
    }
    return true;
#elif defined(__APPLE__)
    // Replacing the range with a fresh anonymous mapping already guarantees
    // zero-filled pages. Pre-zeroing writes every byte immediately before the
    // old mapping is discarded, doubling release traffic without changing the
    // observable contents of a reused Region.
    void* ret = mmap(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ret == MAP_FAILED || ret != reinterpret_cast<void*>(addr)) {
        // Preserve the old failure-path behavior: metadata publication will be
        // rolled back, but a Region returned to the hot pool is still cleared.
        MemorySet(addr, size, 0, size);
        LOG(RTLOG_ERROR,
            "[MS-GC] FreeManager::ReleaseUnits mmap fixed failed: addr=%p size=%zu errno=%d",
            reinterpret_cast<void*>(addr), size, errno);
        return false;
    }
    return true;
#else
    if (madvise(reinterpret_cast<void*>(addr), size, MADV_DONTNEED) != 0) {
        LOG(RTLOG_ERROR, "[MS-GC] FreeManager::ReleaseUnits madvise failed: addr=%p size=%zu errno=%d",
            reinterpret_cast<void*>(addr), size, errno);
        return false;
    }
    return true;
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
