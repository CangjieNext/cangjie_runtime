// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSCentralRegionPool.h"

#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSRegionManager.h"
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Common/BaseObject.h"
#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/SysCall.h"
#include "Base/TimeUtils.h"
#include "CangjieRuntime.h"
#include "Collector/CollectorResources.h"
#include "Heap.h"
#include "Heap/MCollector/BackgroundProcessor.h"
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <thread>

namespace MapleRuntime {

namespace {
constexpr size_t kSweepDeadlineCheckInterval = 4;

void ClearSmallRegionPayload(uintptr_t regionStart)
{
    memset(reinterpret_cast<void*>(regionStart), 0, MS_REGION_SIZE);
}

void TransitionToGlobalEmptyCache(MSRegionInfo* region, const char* context)
{
    MSRegionLocation current = MSRegionInfoArray::GetLocation(region);
    if (current == MSRegionLocation::SWEEP_CLAIMED) {
        MSRegionInfoArray::TransitionState(region,
                                           MSRegionLocation::SWEEP_CLAIMED,
                                           MSRegionLocation::GLOBAL_EMPTY_CACHE,
                                           MSRegionInfoArray::ANY_OWNER,
                                           0,
                                           context);
        return;
    }
    MSRegionInfoArray::TransitionState(region,
                                       MSRegionLocation::TRANSIENT,
                                       MSRegionLocation::GLOBAL_EMPTY_CACHE,
                                       MSRegionInfoArray::ANY_OWNER,
                                       0,
                                       context);
}

}

// Global sweep generation counter (starts at 2 so that sg-2 == 0 is valid initial state)
std::atomic<uint32_t> CentralRegionPool::globalSweepGen_{2};

CentralRegionPool::CentralRegionPool(MSRegionManager* msRegionManager)
    : msRegionManager_(msRegionManager)
{
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        buckets_[cls].BindAggregateCounters(&centralSmallActiveRegions_,
                                            &centralSmallSweptRegions_,
                                            &centralSmallUnsweptRegions_);
    }
}

bool CentralRegionPool::TryEnterSweep()
{
    uint64_t oldState = sweepAdmission_.load(std::memory_order_acquire);
    for (;;) {
        if ((oldState & kSweepAdmissionClosed) != 0) {
            return false;
        }
        CHECK((oldState & kSweepAdmissionActiveMask) != kSweepAdmissionActiveMask);
        uint64_t newState = oldState + 1;
        if (sweepAdmission_.compare_exchange_weak(oldState, newState,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
            return true;
        }
    }
}

void CentralRegionPool::LeaveSweep()
{
    uint64_t oldState = sweepAdmission_.load(std::memory_order_acquire);
    for (;;) {
        CHECK((oldState & kSweepAdmissionActiveMask) != 0);
        uint64_t newState = oldState - 1;
        if (sweepAdmission_.compare_exchange_weak(oldState, newState,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
            return;
        }
    }
}

void CentralRegionPool::CloseSweepAdmissionAndWait()
{
    // This RMW is the single linearization point for closing admission. A
    // worker either increments the active count before this operation and is
    // waited for, or observes the closed bit and cannot enter SweepRegion.
    sweepAdmission_.fetch_or(kSweepAdmissionClosed, std::memory_order_acq_rel);
    while ((sweepAdmission_.load(std::memory_order_acquire) &
            kSweepAdmissionActiveMask) != 0) {
        std::this_thread::yield();
    }
}

void CentralRegionPool::OpenSweepAdmission()
{
    uint64_t state = sweepAdmission_.load(std::memory_order_acquire);
    CHECK((state & kSweepAdmissionActiveMask) == 0);
    CHECK((state & kSweepAdmissionClosed) != 0);
    sweepAdmission_.store(0, std::memory_order_release);
}

void CentralRegionPool::PublishSweepDoneAfterQuiescence()
{
    uint64_t state = sweepAdmission_.load(std::memory_order_acquire);
    CHECK((state & kSweepAdmissionClosed) != 0);
    CHECK((state & kSweepAdmissionActiveMask) == 0);
    CHECK(!forceFinishSweepRunning_.load(std::memory_order_acquire));
    CHECK(!bgSweepRunning_.load(std::memory_order_acquire));
    CHECK(activeLazySweepCount_.load(std::memory_order_acquire) == 0);
    CHECK(activeAssistSweepCount_.load(std::memory_order_acquire) == 0);
    CHECK(activeHelperSweepCount_.load(std::memory_order_acquire) == 0);
    CHECK(GetTotalUnsweptCount() == 0);
    CHECK(GetTotalUnsweptCountReference() == 0);

    // Publish completion first. A concurrent ForceHighSweepPressure() either
    // observes sweepDone_ and returns, or rechecks it after setting the latch
    // and clears its own stale HIGH. Clearing the latch before this store would
    // allow the notifier to leave forced HIGH behind on an already-done epoch.
    sweepDone_.store(true, std::memory_order_release);
    forceHighSweepPressure_.store(false, std::memory_order_release);
    sweepPressureMode_.store(static_cast<int>(SweepPressureMode::LOW), std::memory_order_release);
}

void CentralRegionPool::MaybeNotifySweep(uint8_t cls) const
{
    if (msRegionManager_ != nullptr) {
        msRegionManager_->MaybeNotifySweep(this, cls);
    }
}

std::array<uint8_t, kNumSizeClasses> CentralRegionPool::BuildBackgroundSweepClassOrder(uint8_t preferredCls) const
{
    std::array<MSSweepClassPressure, kNumSizeClasses> pressure {};
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        BucketPressure bucketPressure = GetBucketPressure(cls);
        pressure[cls] = {bucketPressure.ReadyCount(), bucketPressure.UnsweptCount()};
    }
    return BuildMSSweepPressureClassOrder(pressure, preferredCls);
}

// ============================================================================
// TakePartialSweptRegion — fastest central-pool acquisition path.
// ============================================================================
MSRegionInfo* CentralRegionPool::TakePartialSweptRegion(uint8_t cls)
{
    SizeClassBucket& bucket = buckets_[cls];
    MSRegionInfo* region = bucket.TakePartialSwept();
    if (region == nullptr) {
        return nullptr;
    }

    size_t ridx = MSRegionInfoArray::GetRegionIdx(region);
    uintptr_t expectedEnd = MSRegionInfoArray::GetRegionAddress(ridx) + MS_REGION_SIZE;
    if (UNLIKELY(region->regionEnd != expectedEnd)) {
        LOG(RTLOG_FATAL, "[MS-GC] TakePartialSweptRegion: partialSwept region has wrong regionEnd! "
            "regionEnd=0x%zx, expected=0x%zx, allocPtr=0x%zx, cls=%u, "
            "sizeClass=%u, allocBitmap=%p, this=%p (idx=%zu)",
            region->regionEnd, expectedEnd, region->allocPtr,
            (unsigned)cls, (unsigned)region->sizeClass,
            (void*)region->allocBitmap, (void*)region, ridx);
    }
    MSRegionInfoArray::TransitionState(region,
                                       MSRegionLocation::CENTRAL_PARTIAL,
                                       MSRegionLocation::TRANSIENT,
                                       MSRegionInfoArray::ANY_OWNER,
                                       0,
                                       "TakePartialSweptRegion");
    return region;
}

// ============================================================================
// TakeUnsweptRegion — lazily sweep unswept lists and return a usable region.
// Covers partialUnswept first, then fullUnswept.
// ============================================================================
MSRegionInfo* CentralRegionPool::TakeUnsweptRegion(uint8_t cls)
{
    SizeClassBucket& bucket = buckets_[cls];
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    using UnsweptListKind = SizeClassBucket::UnsweptListKind;

    auto SweepUnsweptList = [&](UnsweptListKind kind) -> MSRegionInfo* {
        for (;;) {
            MSRegionInfo* region = bucket.ClaimUnsweptHeadForSweep(kind, sg);
            if (region == nullptr) {
                return nullptr;
            }

            region->SweepRegion();
            region->SetSweepGen(sg);
            RecordLazySweep(1);

            if (region->IsCompletelyFree()) {
                region->ResetToBump();
                MSRegionInfoArray::TransitionState(region,
                                                   MSRegionLocation::SWEEP_CLAIMED,
                                                   MSRegionLocation::TRANSIENT,
                                                   MSRegionInfoArray::ANY_OWNER,
                                                   0,
                                                   "TakeUnsweptRegion empty");
                return region;
            }

            if (kind == UnsweptListKind::PARTIAL || region->HasFreeSpace()) {
                region->regionType = MSRegionType::MS_REGION_PARTIAL;
                MSRegionInfoArray::TransitionState(region,
                                                   MSRegionLocation::SWEEP_CLAIMED,
                                                   MSRegionLocation::TRANSIENT,
                                                   MSRegionInfoArray::ANY_OWNER,
                                                   0,
                                                   "TakeUnsweptRegion partial");
                return region;
            }

            // Still full after sweeping a full-unswept region → move to fullSwept
            region->regionType = MSRegionType::MS_REGION_FULL;
            MSRegionInfoArray::TransitionState(region,
                                               MSRegionLocation::SWEEP_CLAIMED,
                                               MSRegionLocation::CENTRAL_FULL,
                                               MSRegionInfoArray::ANY_OWNER,
                                               0,
                                               "TakeUnsweptRegion repush full");
            bucket.PushFullSwept(region);
        }
    };

    if (!TryEnterSweep()) {
        return nullptr;
    }

    // Notify background sweep only when we actually enter unswept paths,
    // rather than speculatively at central acquisition entry.
    msRegionManager_->MaybeNotifySweep(this, cls);
    activeLazySweepCount_.fetch_add(1, std::memory_order_relaxed);

    if (skipLazySweep_.load(std::memory_order_acquire)) {
        activeLazySweepCount_.fetch_sub(1, std::memory_order_release);
        LeaveSweep();
        return nullptr;
    }

    MSRegionInfo* sweptRegion = SweepUnsweptList(UnsweptListKind::PARTIAL);
    if (sweptRegion == nullptr) {
        sweptRegion = SweepUnsweptList(UnsweptListKind::FULL);
    }
    activeLazySweepCount_.fetch_sub(1, std::memory_order_release);
    LeaveSweep();
    return sweptRegion;
}

// ============================================================================
// GetFreshRegion — allocate a brand-new Region from MSRegionManager
// ============================================================================
MSRegionInfo* CentralRegionPool::GetFreshRegion(uint8_t cls, uint16_t shardHint)
{
    // MSRegionManager::TakeRegion allocates one MS_REGION_SIZE region
    // and returns the corresponding MSRegionInfo* directly (one-level).
    MSRegionInfo* msInfo = msRegionManager_->TakeRegion(shardHint);
    if (msInfo == nullptr) {
        return nullptr;
    }

    // Compute region start address from MSRegionInfo index
    size_t regionIdx = MSRegionInfoArray::GetRegionIdx(msInfo);
    uintptr_t regionStart = MSRegionInfoArray::GetRegionAddress(regionIdx);
    ClearSmallRegionPayload(regionStart);

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (UNLIKELY(msInfo->regionType != MSRegionType::MS_REGION_FREE)) {
        LOG(RTLOG_FATAL, "[MS-GC] GetFreshRegion: TakeRegion returned non-FREE region! "
            "regionType=%d, allocPtr=0x%zx, regionEnd=0x%zx, allocBitmap=%p, "
            "sizeClass=%u, sweepState=%d, sweepGen=%u, "
            "nextIdx=%d, prevIdx=%d, reserved=0x%llx, "
            "cls=%u, regionIdx=%zu, regionStart=0x%zx, this=%p",
            (int)msInfo->regionType, msInfo->allocPtr, msInfo->regionEnd,
            (void*)msInfo->allocBitmap, (unsigned)msInfo->sizeClass,
            (int)msInfo->sweepState, msInfo->sweepGen,
            msInfo->nextRegionIdx, msInfo->prevRegionIdx,
            (unsigned long long)msInfo->reserved,
            (unsigned)cls, regionIdx, regionStart, (void*)msInfo);
    }
#endif

    // Initialize for the requested size class (sets bitmaps, slot counts, state)
    msInfo->InitForSizeClass(cls);

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (UNLIKELY(msInfo->allocBitmap == nullptr)) {
        LOG(RTLOG_FATAL, "[MS-GC] GetFreshRegion: InitForSizeClass failed to alloc bitmap, "
            "cls=%u, regionIdx=%zu, regionStart=0x%zx, totalSlots=%u, this=%p",
            (unsigned)cls, regionIdx, regionStart, (unsigned)msInfo->totalSlotCount,
            (void*)msInfo);
    }
#endif

    // Stamp the current sweep generation so this Region is seen as "already swept"
    msInfo->SetSweepGen(globalSweepGen_.load(std::memory_order_acquire));
    MSRegionInfoArray::TransitionState(msInfo,
                                       MSRegionLocation::FREE,
                                       MSRegionLocation::TRANSIENT,
                                       MSRegionInfoArray::ANY_OWNER,
                                       0,
                                       "GetFreshRegion");

    return msInfo;
}

uint16_t CentralRegionPool::ResolveHomeShardId(uint16_t shardHint) const
{
    return msRegionManager_ != nullptr ? msRegionManager_->ResolveHomeShardId(shardHint) : 0;
}

// ============================================================================
// Return / Release — Region return from thread-local cache
// ============================================================================

void CentralRegionPool::ReturnPartialRegion(MSRegionInfo* region, uint8_t cls)
{
    CHECK_DETAIL(region != nullptr, "ReturnPartialRegion with null region");
    uint8_t bucketCls = region->sizeClass;
    CHECK_DETAIL(bucketCls < kNumSizeClasses,
                 "[MS-GC] ReturnPartialRegion: invalid sizeClass=%u",
                 static_cast<unsigned>(bucketCls));
    if (UNLIKELY(cls != bucketCls)) {
        LOG(RTLOG_ERROR,
            "[MS-GC] ReturnPartialRegion: caller cls=%u mismatches region sizeClass=%u, region=%p",
            static_cast<unsigned>(cls),
            static_cast<unsigned>(bucketCls),
            static_cast<void*>(region));
    }
    SizeClassBucket& bucket = buckets_[bucketCls];
    region->regionType = MSRegionType::MS_REGION_PARTIAL;
    MSRegionInfoArray::TransitionState(region,
                                       MSRegionLocation::TRANSIENT,
                                       MSRegionLocation::CENTRAL_PARTIAL,
                                       MSRegionInfoArray::ANY_OWNER,
                                       0,
                                       "ReturnPartialRegion");
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    if (region->GetSweepGen() == sg) {
        bucket.PushPartialSwept(region);
    } else {
        bucket.PushPartialUnswept(region);
    }
}

void CentralRegionPool::ReturnFullRegion(MSRegionInfo* region, uint8_t cls)
{
    CHECK_DETAIL(region != nullptr, "ReturnFullRegion with null region");
    uint8_t bucketCls = region->sizeClass;
    CHECK_DETAIL(bucketCls < kNumSizeClasses,
                 "[MS-GC] ReturnFullRegion: invalid sizeClass=%u",
                 static_cast<unsigned>(bucketCls));
    if (UNLIKELY(cls != bucketCls)) {
        LOG(RTLOG_ERROR,
            "[MS-GC] ReturnFullRegion: caller cls=%u mismatches region sizeClass=%u, region=%p",
            static_cast<unsigned>(cls),
            static_cast<unsigned>(bucketCls),
            static_cast<void*>(region));
    }
    SizeClassBucket& bucket = buckets_[bucketCls];
    region->regionType = MSRegionType::MS_REGION_FULL;
    MSRegionInfoArray::TransitionState(region,
                                       MSRegionLocation::TRANSIENT,
                                       MSRegionLocation::CENTRAL_FULL,
                                       MSRegionInfoArray::ANY_OWNER,
                                       0,
                                       "ReturnFullRegion");
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    if (region->GetSweepGen() == sg) {
        bucket.PushFullSwept(region);
    } else {
        bucket.PushFullUnswept(region);
    }
}

void CentralRegionPool::ReturnEmptyRegion(MSRegionInfo* region)
{
    PushEmptySweptRegion(region);
}

void CentralRegionPool::PushEmptySweptRegion(MSRegionInfo* region)
{
    CHECK_DETAIL(region != nullptr, "PushEmptySweptRegion with null region");
    region->ResetToEmptyCache();
    TransitionToGlobalEmptyCache(region, "PushEmptySweptRegion");
    {
        std::lock_guard<std::mutex> lg(emptySweptLock_);
        emptySweptCache_.Prepend(region);
    }
    IncrementEmptySweptCount();
}

MSRegionInfo* CentralRegionPool::TakeGlobalEmptyRegion()
{
    MSRegionInfo* region = nullptr;
    {
        std::lock_guard<std::mutex> lg(emptySweptLock_);
        region = emptySweptCache_.TakeHead();
    }
    if (region != nullptr) {
        DecrementEmptySweptCount();
        MSRegionInfoArray::TransitionState(region,
                                           MSRegionLocation::GLOBAL_EMPTY_CACHE,
                                           MSRegionLocation::TRANSIENT,
                                           MSRegionInfoArray::ANY_OWNER,
                                           0,
                                           "TakeGlobalEmptyRegion");
    }
    return region;
}

size_t CentralRegionPool::DrainEmptySweptGlobal(size_t maxRegions, std::vector<MSRegionInfo*>& drained)
{
    if (maxRegions == 0) {
        return 0;
    }

    constexpr size_t kEmptyDrainBatchSize = 64;
    size_t startSize = drained.size();
    std::vector<MSRegionInfo*> batch;
    batch.reserve(std::min(maxRegions, kEmptyDrainBatchSize));

    while (drained.size() - startSize < maxRegions) {
        batch.clear();
        {
            std::lock_guard<std::mutex> lg(emptySweptLock_);
            size_t batchLimit = std::min(maxRegions - (drained.size() - startSize), kEmptyDrainBatchSize);
            for (size_t i = 0; i < batchLimit; ++i) {
                MSRegionInfo* region = emptySweptCache_.TakeHead();
                if (region == nullptr) {
                    break;
                }
                batch.push_back(region);
            }
        }
        if (batch.empty()) {
            break;
        }
        DecrementEmptySweptCount(batch.size());
        for (MSRegionInfo* region : batch) {
            MSRegionInfoArray::TransitionState(region,
                                               MSRegionLocation::GLOBAL_EMPTY_CACHE,
                                               MSRegionLocation::TRANSIENT,
                                               MSRegionInfoArray::ANY_OWNER,
                                               0,
                                               "DrainEmptySweptGlobal");
        }
        drained.insert(drained.end(), batch.begin(), batch.end());
    }

    return drained.size() - startSize;
}
size_t CentralRegionPool::ReclaimEmptyRegion(size_t maxRegions)
{
    size_t emptyCount = GetEmptySweptRegionCount();
    if (emptyCount == 0 || maxRegions == 0) {
        return 0;
    }

    std::vector<MSRegionInfo*> reclaimed;
    size_t reserveCount = std::min(emptyCount, maxRegions);
    if (reserveCount == std::numeric_limits<size_t>::max()) {
        reserveCount = 0;
    }
    reclaimed.reserve(reserveCount);
    DrainEmptySweptGlobal(maxRegions, reclaimed);
    if (reclaimed.size() < maxRegions) {
        std::lock_guard<std::mutex> lg(msAllocBufferLock_);
        for (AllocBufferMS* buffer : msAllocBuffers_) {
            if (buffer == nullptr) {
                continue;
            }
            size_t remaining = maxRegions - reclaimed.size();
            if (remaining == 0) {
                break;
            }
            (void)buffer->DrainLocalEmptyRegions(reclaimed, remaining);
        }
    }

    if (reclaimed.size() > maxRegions) {
        for (size_t i = maxRegions; i < reclaimed.size(); ++i) {
            PushEmptySweptRegion(reclaimed[i]);
        }
        reclaimed.resize(maxRegions);
    }

    size_t promotedBytes = 0;
    std::vector<FreeManager::RegionSpan> spans;
    spans.reserve(reclaimed.size());
    for (MSRegionInfo* region : reclaimed) {
        size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
        CHECK_DETAIL(region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE,
                     "[MS-GC] ReclaimEmptyRegion: bad regionType=%u idx=%zu",
                     static_cast<unsigned>(region->regionType), regionIdx);
        region->Reset();
        spans.push_back({ static_cast<FreeManager::RegionIndex>(regionIdx), 1 });
        promotedBytes += MS_REGION_SIZE;
    }
    msRegionManager_->ReclaimReleasedRegionsToHotFreeBatch(spans);

    return promotedBytes;
}

bool CentralRegionPool::HasIdleSegmentCachedRegions(size_t startRegionIdx, size_t endRegionIdx) const
{
    if (startRegionIdx >= endRegionIdx || msRegionManager_ == nullptr) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(emptySweptLock_));
        bool found = false;
        emptySweptCache_.ForEach([&](MSRegionInfo* region) {
            if (found || region == nullptr) {
                return;
            }
            size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
            if (regionIdx >= startRegionIdx && regionIdx < endRegionIdx) {
                found = true;
            }
        });
        if (found) {
            return true;
        }
    }

    {
        std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
        for (AllocBufferMS* buffer : msAllocBuffers_) {
            if (buffer != nullptr && buffer->HasLocalEmptyRegionInRange(startRegionIdx, endRegionIdx)) {
                return true;
            }
        }
    }

    uintptr_t rangeStart = MSRegionInfoArray::GetRegionAddress(startRegionIdx);
    uintptr_t rangeEnd = MSRegionInfoArray::GetRegionAddress(endRegionIdx);
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(largeHotCacheLock_));
    for (const auto& cacheEntry : largeHotCache_) {
        const auto& bucket = cacheEntry.second;
        for (const LargeHotEntry& entry : bucket) {
            uintptr_t entryStart = entry.regionStart;
            uintptr_t entryEnd = entry.regionStart + entry.regionBytes;
            if (entryStart < rangeEnd && rangeStart < entryEnd) {
                return true;
            }
        }
    }
    return false;
}

size_t CentralRegionPool::ReclaimIdleSegmentCaches(size_t startRegionIdx, size_t endRegionIdx)
{
    if (startRegionIdx >= endRegionIdx || msRegionManager_ == nullptr) {
        return 0;
    }

    std::vector<MSRegionInfo*> reclaimedEmptyRegions;
    {
        std::vector<MSRegionInfo*> kept;
        std::lock_guard<std::mutex> lg(emptySweptLock_);
        for (;;) {
            MSRegionInfo* region = emptySweptCache_.TakeHead();
            if (region == nullptr) {
                break;
            }
            size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
            if (regionIdx >= startRegionIdx && regionIdx < endRegionIdx) {
                reclaimedEmptyRegions.push_back(region);
                continue;
            }
            kept.push_back(region);
        }
        for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
            emptySweptCache_.Prepend(*it);
        }
    }
    if (!reclaimedEmptyRegions.empty()) {
        DecrementEmptySweptCount(reclaimedEmptyRegions.size());
    }

    {
        std::lock_guard<std::mutex> lg(msAllocBufferLock_);
        for (AllocBufferMS* buffer : msAllocBuffers_) {
            if (buffer == nullptr) {
                continue;
            }
            (void)buffer->ExtractLocalEmptyRegionsInRange(reclaimedEmptyRegions, startRegionIdx, endRegionIdx,
                                                          std::numeric_limits<size_t>::max());
        }
    }

    size_t promotedBytes = 0;
    if (!reclaimedEmptyRegions.empty()) {
        std::vector<FreeManager::RegionSpan> spans;
        spans.reserve(reclaimedEmptyRegions.size());
        for (MSRegionInfo* region : reclaimedEmptyRegions) {
            size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
            CHECK_DETAIL(region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE,
                         "[MS-GC] ReclaimIdleSegmentCaches: bad empty-cache regionType=%u idx=%zu",
                         static_cast<unsigned>(region->regionType), regionIdx);
            region->Reset();
            spans.push_back({ static_cast<FreeManager::RegionIndex>(regionIdx), 1 });
            promotedBytes += MS_REGION_SIZE;
        }
        msRegionManager_->ReclaimReleasedRegionsToHotFreeBatch(spans);
    }

    uintptr_t rangeStart = MSRegionInfoArray::GetRegionAddress(startRegionIdx);
    uintptr_t rangeEnd = MSRegionInfoArray::GetRegionAddress(endRegionIdx);
    std::vector<LargeHotEntry> reclaimedLargeEntries;
    size_t reclaimedLargeRegions = 0;
    size_t reclaimedLargeBytes = 0;
    {
        std::lock_guard<std::mutex> lg(largeHotCacheLock_);
        for (auto it = largeHotCache_.begin(); it != largeHotCache_.end();) {
            auto& bucket = it->second;
            for (auto entryIt = bucket.begin(); entryIt != bucket.end();) {
                uintptr_t entryStart = entryIt->regionStart;
                uintptr_t entryEnd = entryIt->regionStart + entryIt->regionBytes;
                if (entryStart < rangeEnd && rangeStart < entryEnd) {
                    reclaimedLargeBytes += entryIt->regionBytes;
                    reclaimedLargeRegions += it->first;
                    reclaimedLargeEntries.push_back(*entryIt);
                    entryIt = bucket.erase(entryIt);
                } else {
                    ++entryIt;
                }
            }
            if (bucket.empty()) {
                it = largeHotCache_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (reclaimedLargeBytes != 0) {
        largeHotCacheRegionCount_.fetch_sub(reclaimedLargeRegions, std::memory_order_release);
        largeHotCacheBytes_.fetch_sub(reclaimedLargeBytes, std::memory_order_release);
        for (const LargeHotEntry& entry : reclaimedLargeEntries) {
            msRegionManager_->ReclaimRegionLarge(entry.regionStart, entry.regionBytes);
            promotedBytes += entry.regionBytes;
        }
    }

    return promotedBytes;
}

// ============================================================================
// GC operations
// ============================================================================

void CentralRegionPool::PrepareForSweep()
{
    // At this point MarkSetup has already stopped sweep and promoted any residual
    // unswept regions to swept. FlushAllAllocBuffers returns TL regions to swept.
    // O(1) per-bucket flip:
    //   partialSwept → partialUnswept (StealChainTo: head pointer transfer)
    //   fullSwept    ↔ fullUnswept    (Swap: head/count exchange)
    // Count AFTER the flip: FlipSweptToUnswept moves the regions that need sweeping
    // this cycle from the swept lists into the unswept lists. Counting before the
    // flip would read the previous cycle's residual unswept lists (already drained
    // to ~0 by ForceFinishAllUnswept), yielding totalUnswept==0 and a zero ratio.
    size_t totalUnswept = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        buckets_[cls].FlipSweptToUnswept();
        totalUnswept += buckets_[cls].CountPartialUnswept() + buckets_[cls].CountFullUnswept();
    }
    centralSmallUnsweptRegions_.store(totalUnswept, std::memory_order_release);

    // Reset the per-cycle central credit and publish the fixed-point proportional
    // Sweep ratio once. Allocation progress and debt live in each AllocBufferMS.
    // runway = how much more allocation can happen before the next GC trigger,
    // measured on the swept-region-bytes axis the HEU trigger actually compares
    // against (threshold vs GetSweptHeapBytes), consistent with
    // BackgroundProcessor::HasPendingSweepWork. At cycle start sweptBytes is ~0 so
    // runway ~= threshold. threshold is stable within a cycle (only changed at GC
    // boundaries), so the ratio is fixed here and read without re-computation.
    forceHighSweepPressure_.store(false, std::memory_order_relaxed);
    sweepPressureMode_.store(static_cast<int>(SweepPressureMode::LOW), std::memory_order_relaxed);
    sweepCredit_.Reset();
    {
        RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
        size_t threshold = space.GetHeapThresholdBytes();
        size_t sweptBytes = space.GetSweptHeapBytes();
        size_t runwayRegions = ComputeRunwayRegions(threshold, sweptBytes, MS_REGION_SIZE);
        sweepRegionsPerAllocScaled_.store(
            ComputeMSSweepAssistRatioScaled(totalUnswept, runwayRegions),
            std::memory_order_relaxed);
    }

    globalSweepGen_.fetch_add(2, std::memory_order_release);
    sweepDone_.store(false, std::memory_order_release);
}

// ============================================================================
// BackgroundSweep — called by gc-helper thread (BackgroundProcessor)
// ============================================================================

void CentralRegionPool::BackgroundSweep(uint8_t preferredCls, uint64_t deadlineUs)
{
    if (!TryEnterSweep()) {
        return;
    }
    bgSweepRunning_.store(true, std::memory_order_release);
    auto finish = [this]() {
        bgSweepRunning_.store(false, std::memory_order_release);
        LeaveSweep();
    };

    if (stopSweepRequested_.load(std::memory_order_acquire)) {
        finish();
        return;
    }

    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    size_t remainingUnswept = GetTotalUnsweptCount();
    if (remainingUnswept == 0) {
        finish();
        return;
    }
    size_t sweptInSlice = 0;

    auto SweepBucket = [&](uint8_t cls) -> bool {
        SizeClassBucket& bucket = buckets_[cls];
        if (!SweepUnsweptList(bucket, SizeClassBucket::UnsweptListKind::PARTIAL,
                              sg, sweptInSlice, SIZE_MAX, deadlineUs)) {
            return false;
        }
        if (!SweepUnsweptList(bucket, SizeClassBucket::UnsweptListKind::FULL,
                              sg, sweptInSlice, SIZE_MAX, deadlineUs)) {
            return false;
        }
        return true;
    };

    auto classOrder = BuildBackgroundSweepClassOrder(preferredCls);
    for (uint8_t cls : classOrder) {
        if (!SweepBucket(cls)) {
            break;
        }
    }

    RecordBackgroundSweep(sweptInSlice);
    finish();
}

void CentralRegionPool::ParallelSweep(uint8_t preferredCls, uint64_t deadlineUs)
{
    if (!TryEnterSweep()) {
        return;
    }
    activeHelperSweepCount_.fetch_add(1, std::memory_order_relaxed);
    if (skipHelperSweep_.load(std::memory_order_acquire) ||
        stopSweepRequested_.load(std::memory_order_acquire)) {
        activeHelperSweepCount_.fetch_sub(1, std::memory_order_release);
        LeaveSweep();
        return;
    }

    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    size_t sweptInSlice = 0;
    auto classOrder = BuildMSSweepCyclicClassOrder<kNumSizeClasses>(preferredCls);
    for (uint8_t cls : classOrder) {
        SizeClassBucket& bucket = buckets_[cls];
        if (!SweepUnsweptList(bucket, SizeClassBucket::UnsweptListKind::PARTIAL,
                              sg, sweptInSlice, SIZE_MAX, deadlineUs)) {
            break;
        }
        if (!SweepUnsweptList(bucket, SizeClassBucket::UnsweptListKind::FULL,
                              sg, sweptInSlice, SIZE_MAX, deadlineUs)) {
            break;
        }
    }

    RecordHelperSweep(sweptInSlice);
    activeHelperSweepCount_.fetch_sub(1, std::memory_order_release);
    LeaveSweep();
}

MSSweepAssistResult CentralRegionPool::MSSweepAssist(AllocBufferMS& owner, size_t debtRegions,
                                                     uint8_t preferredCls)
{
    MSSweepAssistResult result;
    if (debtRegions == 0 || sweepDone_.load(std::memory_order_acquire) ||
        GetTotalUnsweptCount() == 0) {
        result.sweepComplete = sweepDone_.load(std::memory_order_acquire);
        return result;
    }

    // Background/lazy/helper work is a one-shot global credit. CAS claiming is
    // the ownership transfer: once one P consumes it, no other P can use the
    // same swept Region to reduce its local debt.
    result.creditPaidRegions = sweepCredit_.Take(debtRegions);
    size_t remainingDebt = debtRegions - result.creditPaidRegions;
    if (remainingDebt == 0) {
        result.sweepComplete = sweepDone_.load(std::memory_order_acquire);
        return result;
    }

    // Pressure scales debt addition, never repayment speed. Retain a sub-batch
    // remainder; once it reaches the configured threshold, run one exact batch.
    size_t toSweep = ComputeMSSweepAssistDirectRegions(
        remainingDebt, CangjieRuntime::GetGCParam().msAssistSweepMaxRegionsPerCall);
    if (toSweep == 0) {
        result.sweepComplete = sweepDone_.load(std::memory_order_acquire);
        return result;
    }

    // AssistSweep carries the unified admission guard. GC closes that guard
    // before clearing mark bitmaps, so no new SweepRegion can start afterward.
    result.assistPaidRegions = AssistSweep(toSweep, preferredCls, &owner);
    result.sweepComplete = sweepDone_.load(std::memory_order_acquire);
    return result;
}

SweepPressureMode CentralRegionPool::RefreshSweepDebtSnapshot(size_t thresholdBytes, size_t sweptBytes)
{
    size_t unsweptBytes = GetTotalUnsweptBytes();
    bool forcedHigh = forceHighSweepPressure_.load(std::memory_order_acquire);
    SweepPressureMode mode = sweepDone_.load(std::memory_order_acquire) || unsweptBytes == 0
        ? SweepPressureMode::LOW
        : EvaluateSweepPressure(unsweptBytes, sweptBytes, thresholdBytes, forcedHigh);
    if (unsweptBytes == 0) {
        forceHighSweepPressure_.store(false, std::memory_order_release);
    }
    sweepPressureMode_.store(static_cast<int>(mode), std::memory_order_release);

    return mode;
}

void CentralRegionPool::ForceHighSweepPressure()
{
    if (sweepDone_.load(std::memory_order_acquire) || GetTotalUnsweptCount() == 0) {
        return;
    }
    forceHighSweepPressure_.store(true, std::memory_order_release);
    sweepPressureMode_.store(static_cast<int>(SweepPressureMode::HIGH), std::memory_order_release);
    // Close the race where the final Sweep publisher completed immediately
    // before these stores. A completed epoch must never retain forced HIGH.
    if (sweepDone_.load(std::memory_order_acquire)) {
        forceHighSweepPressure_.store(false, std::memory_order_release);
        sweepPressureMode_.store(static_cast<int>(SweepPressureMode::LOW), std::memory_order_release);
    }
}

size_t CentralRegionPool::AssistSweep(size_t targetRegions, uint8_t preferredCls, AllocBufferMS* preferredOwner)
{
    if (targetRegions == 0) {
        return 0;
    }

    if (!TryEnterSweep()) {
        return 0;
    }
    activeAssistSweepCount_.fetch_add(1, std::memory_order_relaxed);
    if (skipAssistSweep_.load(std::memory_order_acquire) ||
        stopSweepRequested_.load(std::memory_order_acquire)) {
        activeAssistSweepCount_.fetch_sub(1, std::memory_order_release);
        LeaveSweep();
        return 0;
    }

    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    size_t sweptRegions = 0;
    auto classOrder = BuildMSSweepCyclicClassOrder<kNumSizeClasses>(preferredCls);
    for (uint8_t cls : classOrder) {
        SizeClassBucket& bucket = buckets_[cls];
        while (sweptRegions < targetRegions &&
               SweepOneUnsweptRegionForAssist(bucket, SizeClassBucket::UnsweptListKind::PARTIAL,
                                             sg, preferredOwner)) {
            ++sweptRegions;
        }
        while (sweptRegions < targetRegions &&
               SweepOneUnsweptRegionForAssist(bucket, SizeClassBucket::UnsweptListKind::FULL,
                                             sg, preferredOwner)) {
            ++sweptRegions;
        }
        if (sweptRegions >= targetRegions) {
            break;
        }
    }
    activeAssistSweepCount_.fetch_sub(1, std::memory_order_release);
    LeaveSweep();
    return sweptRegions;
}

bool CentralRegionPool::SweepOneUnsweptRegionForAssist(SizeClassBucket& bucket,
                                                       SizeClassBucket::UnsweptListKind listKind,
                                                       uint32_t sg,
                                                       AllocBufferMS* preferredOwner,
                                                       size_t* emptiedInSlice)
{
    for (;;) {
        MSRegionInfo* region = bucket.ClaimUnsweptHeadForSweep(listKind, sg);
        if (region == nullptr) {
            return false;
        }

        region->SweepRegion();
        region->SetSweepGen(sg);
        if (region->IsCompletelyFree()) {
            bool pushedLocal = preferredOwner != nullptr && preferredOwner->TryPushLocalEmptyRegion(region);
            if (!pushedLocal) {
                PushEmptySweptRegion(region);
            }
            if (emptiedInSlice != nullptr) {
                (*emptiedInSlice)++;
            }
        } else if (region->HasFreeSpace()) {
            region->regionType = MSRegionType::MS_REGION_PARTIAL;
            MSRegionInfoArray::TransitionState(region,
                                               MSRegionLocation::SWEEP_CLAIMED,
                                               MSRegionLocation::CENTRAL_PARTIAL,
                                               MSRegionInfoArray::ANY_OWNER,
                                               0,
                                               "SweepOneUnsweptRegionForAssist partial");
            bucket.PushPartialSwept(region);
        } else {
            region->regionType = MSRegionType::MS_REGION_FULL;
            MSRegionInfoArray::TransitionState(region,
                                               MSRegionLocation::SWEEP_CLAIMED,
                                               MSRegionLocation::CENTRAL_FULL,
                                               MSRegionInfoArray::ANY_OWNER,
                                               0,
                                               "SweepOneUnsweptRegionForAssist full");
            bucket.PushFullSwept(region);
        }
        return true;
    }
}

bool CentralRegionPool::SweepUnsweptList(SizeClassBucket& bucket, SizeClassBucket::UnsweptListKind listKind,
                                         uint32_t sg,
                                         size_t& sweptInSlice, size_t maxRegions, uint64_t deadlineUs,
                                         size_t* emptiedInSlice)
{
    for (;;) {
        if (sweptInSlice >= maxRegions) {
            return false;
        }
        if (sweptInSlice % kSweepDeadlineCheckInterval == 0 &&
            TimeUtil::MicroSeconds() >= deadlineUs) {
            return false;
        }

        MSRegionInfo* region = bucket.ClaimUnsweptHeadForSweep(listKind, sg);
        if (region == nullptr) {
            break;
        }

        region->SweepRegion();
        region->SetSweepGen(sg);
        sweptInSlice++;

        if (region->IsCompletelyFree()) {
            // Completely empty regions leave the class-local supply and
            // become classless empty-cache entries.
            PushEmptySweptRegion(region);
            if (emptiedInSlice != nullptr) {
                (*emptiedInSlice)++;
            }
        } else if (region->HasFreeSpace()) {
            region->regionType = MSRegionType::MS_REGION_PARTIAL;
            MSRegionInfoArray::TransitionState(region,
                                               MSRegionLocation::SWEEP_CLAIMED,
                                               MSRegionLocation::CENTRAL_PARTIAL,
                                               MSRegionInfoArray::ANY_OWNER,
                                               0,
                                               "SweepUnsweptList partial");
            bucket.PushPartialSwept(region);
        } else {
            region->regionType = MSRegionType::MS_REGION_FULL;
            MSRegionInfoArray::TransitionState(region,
                                               MSRegionLocation::SWEEP_CLAIMED,
                                               MSRegionLocation::CENTRAL_FULL,
                                               MSRegionInfoArray::ANY_OWNER,
                                               0,
                                               "SweepUnsweptList full");
            bucket.PushFullSwept(region);
        }

    }

    return true;
}

// ===== Monitoring =====

size_t CentralRegionPool::GetTotalUnsweptCount() const
{
    return centralSmallUnsweptRegions_.load(std::memory_order_acquire);
}

size_t CentralRegionPool::GetTotalUnsweptCountReference() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        total += buckets_[cls].CountPartialUnswept();
        total += buckets_[cls].CountFullUnswept();
    }
    return total;
}

size_t CentralRegionPool::GetTotalUnsweptBytes() const
{
    uint64_t bytes = SaturatingMultiplyMSSweepValue(
        static_cast<uint64_t>(GetTotalUnsweptCount()), static_cast<uint64_t>(MS_REGION_SIZE));
    return bytes > std::numeric_limits<size_t>::max()
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(bytes);
}

size_t CentralRegionPool::GetLargeRegionCount() const
{
    return largeRegionList_.Count();
}

void CentralRegionPool::ForceFinishUnsweptList(SizeClassBucket& bucket, SizeClassBucket::UnsweptListKind listKind,
                                               uint32_t sg, size_t& sweptRegions, size_t& emptiedRegions,
                                               size_t& retainedRegions, size_t& freedBytes)
{
    for (;;) {
        MSRegionInfo* region = bucket.ClaimUnsweptHeadForSweep(listKind, sg);
        if (region == nullptr) {
            break;
        }

        size_t regionFreedBytes = region->SweepRegion();
        region->SetSweepGen(sg);
        sweptRegions++;
        freedBytes += regionFreedBytes;

        if (region->IsCompletelyFree()) {
            emptiedRegions++;
            PushEmptySweptRegion(region);
            continue;
        }

        retainedRegions++;
        if (region->HasFreeSpace()) {
            region->regionType = MSRegionType::MS_REGION_PARTIAL;
            MSRegionInfoArray::TransitionState(region,
                                               MSRegionLocation::SWEEP_CLAIMED,
                                               MSRegionLocation::CENTRAL_PARTIAL,
                                               MSRegionInfoArray::ANY_OWNER,
                                               0,
                                               "ForceFinishUnsweptList partial");
            bucket.PushPartialSwept(region);
            continue;
        }

        region->regionType = MSRegionType::MS_REGION_FULL;
        MSRegionInfoArray::TransitionState(region,
                                           MSRegionLocation::SWEEP_CLAIMED,
                                           MSRegionLocation::CENTRAL_FULL,
                                           MSRegionInfoArray::ANY_OWNER,
                                           0,
                                           "ForceFinishUnsweptList full");
        bucket.PushFullSwept(region);
    }
}

void CentralRegionPool::ForceFinishAllUnswept()
{
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    size_t sweptRegions = 0;
    size_t emptiedRegions = 0;
    size_t retainedRegions = 0;
    size_t freedBytes = 0;

    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket& bucket = buckets_[cls];
        ForceFinishUnsweptList(bucket, SizeClassBucket::UnsweptListKind::PARTIAL, sg,
                            sweptRegions, emptiedRegions, retainedRegions, freedBytes);
        ForceFinishUnsweptList(bucket, SizeClassBucket::UnsweptListKind::FULL, sg,
                            sweptRegions, emptiedRegions, retainedRegions, freedBytes);
    }

    VLOG(REPORT,
        "[MS-GC] ForceFinishAllUnswept: swept=%zu regions, emptied=%zu, retained=%zu, freed=%s B",
        sweptRegions, emptiedRegions, retainedRegions, Pretty(freedBytes).Str());
}

void CentralRegionPool::ForceFinishAllUnsweptForOOM()
{
    // PreEnumRoots closed these gates before Mark and the OOM path deliberately
    // did not reopen them in MarkTermination. Therefore GC main is the only
    // thread that can claim a Region from this cycle's unswept lists here.
    CHECK(stopSweepRequested_.load(std::memory_order_acquire));
    CHECK(skipLazySweep_.load(std::memory_order_acquire));
    CHECK(skipAssistSweep_.load(std::memory_order_acquire));
    CHECK(skipHelperSweep_.load(std::memory_order_acquire));

    forceFinishSweepRunning_.store(true, std::memory_order_release);
    size_t unsweptBefore = GetTotalUnsweptCount();
    ForceFinishAllUnswept();
    CHECK(GetTotalUnsweptCount() == 0);
    CHECK(GetTotalUnsweptCountReference() == 0);
    forceFinishSweepRunning_.store(false, std::memory_order_release);
    // OOM force-finish is an emergency allocation path, not normal completion
    // of this sweep epoch. Keep sweepDone_ unchanged; only the normal GC
    // StopAndWaitForAllSweep path may publish it.
    forceHighSweepPressure_.store(false, std::memory_order_release);
    sweepPressureMode_.store(static_cast<int>(SweepPressureMode::LOW), std::memory_order_release);
    VLOG(REPORT,
         "[MS-GC] OOM force sweep: unswept=%zu->0 emptyCacheRegions=%zu largeCacheRegions=%zu",
         unsweptBefore, GetEmptySweptRegionCount(), GetLargeHotCacheRegionCount());
}

size_t CentralRegionPool::CountTLActiveRegions() const
{
    size_t count = 0;
    // VisitAllocBufferMS requires non-const; safe to const_cast for read-only counting
    const_cast<CentralRegionPool*>(this)->VisitAllocBufferMS([&count](AllocBufferMS& buf) {
        for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
            if (buf.GetActiveRegion(cls) != nullptr) {
                count++;
            }
        }
    });
    return count;
}

size_t CentralRegionPool::GetAllocBufferCount() const
{
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
    return msAllocBuffers_.size();
}

size_t CentralRegionPool::GetAllAllocBufferActiveRegionCount() const
{
    size_t total = 0;
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
    for (AllocBufferMS* buffer : msAllocBuffers_) {
        if (buffer != nullptr) {
            total += buffer->CountActiveRegions();
        }
    }
    return total;
}

size_t CentralRegionPool::GetSweptMSRegionCount() const
{
    return centralSmallSweptRegions_.load(std::memory_order_relaxed) +
           largeLiveSpanCount_.load(std::memory_order_relaxed);
}

size_t CentralRegionPool::GetSweptMSRegionCountReference() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        total += buckets_[cls].CountPartialSwept();
        total += buckets_[cls].CountFullSwept();
    }
    total += largeLiveSpanCount_.load(std::memory_order_acquire);
    return total;
}

size_t CentralRegionPool::GetActiveMSRegionCount() const
{
    return centralSmallActiveRegions_.load(std::memory_order_relaxed) +
           largeLiveSpanCount_.load(std::memory_order_relaxed);
}

size_t CentralRegionPool::GetActiveMSRegionCountReference() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        total += buckets_[cls].CountPartialSwept();
        total += buckets_[cls].CountFullSwept();
        total += buckets_[cls].CountPartialUnswept();
        total += buckets_[cls].CountFullUnswept();
    }
    total += largeLiveSpanCount_.load(std::memory_order_acquire);
    return total;
}

size_t CentralRegionPool::GetEmptySweptRegionCount() const
{
    return emptySweptCount_.load(std::memory_order_acquire);
}

size_t CentralRegionPool::GetLargeHotCacheRegionCount() const
{
    return largeHotCacheRegionCount_.load(std::memory_order_acquire);
}

CentralRegionPool::BucketPressure CentralRegionPool::GetBucketPressure(uint8_t cls) const
{
    return buckets_[cls].GetPressure();
}

void CentralRegionPool::DumpPoolStats() const
{
    size_t partialSweptTotal = 0;
    size_t fullSweptTotal = 0;
    size_t partialUnsweptTotal = 0;
    size_t fullUnsweptTotal = 0;
    size_t emptySweptTotal = GetEmptySweptRegionCount();

    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        partialSweptTotal += buckets_[cls].CountPartialSwept();
        fullSweptTotal += buckets_[cls].CountFullSwept();
        partialUnsweptTotal += buckets_[cls].CountPartialUnswept();
        fullUnsweptTotal += buckets_[cls].CountFullUnswept();
    }

    size_t totalSmall = partialSweptTotal + fullSweptTotal + partialUnsweptTotal + fullUnsweptTotal +
                        emptySweptTotal;

    VLOG(REPORT, "[MS-GC]   CentralPool small (%u classes aggregated):", kNumSizeClasses);
    VLOG(REPORT, "[MS-GC]     partialSwept: %zu regions  fullSwept: %zu regions",
         partialSweptTotal, fullSweptTotal);
    VLOG(REPORT, "[MS-GC]     partialUnswept: %zu regions  fullUnswept: %zu regions",
         partialUnsweptTotal, fullUnsweptTotal);
    VLOG(REPORT, "[MS-GC]     emptySwept(global+local): %zu regions", emptySweptTotal);
    VLOG(REPORT, "[MS-GC]     total small: %zu regions", totalSmall);
    VLOG(REPORT, "[MS-GC]     large regions: %zu", GetLargeRegionCount());
    size_t largeCacheRegions = GetLargeHotCacheRegionCount();
    VLOG(REPORT, "[MS-GC]     largeHotCache: %zu regions (%zu KB)",
         largeCacheRegions, (largeCacheRegions * MS_REGION_SIZE) / 1024);
}

// ============================================================================
// Large Region management (Tier 1 in-heap large objects)
// ============================================================================

void CentralRegionPool::AddLargeLiveRegion(MSRegionInfo* region)
{
    size_t spanCount = region->LargeSpanCount();
    MSRegionInfoArray::SetState(region, MSRegionLocation::LARGE_ACTIVE, 0);
    std::lock_guard<std::mutex> lg(largeLock_);
    largeRegionList_.Prepend(region);
    largeLiveSpanCount_.fetch_add(spanCount, std::memory_order_release);
    largeLiveObjectBytes_.fetch_add(region->LargeObjectSize(), std::memory_order_release);
}

void CentralRegionPool::RegisterLargeRegion(MSRegionInfo* region)
{
    AddLargeLiveRegion(region);
}

void CentralRegionPool::CollectDeadLargeRegions(std::vector<MSRegionInfo*>& deadRegions)
{
    std::lock_guard<std::mutex> lg(largeLock_);
    std::vector<MSRegionInfo*> toRemove;
    largeRegionList_.ForEach([&toRemove](MSRegionInfo* region) {
        if (!region->IsMarked()) {
            toRemove.push_back(region);
        }
    });
    for (MSRegionInfo* region : toRemove) {
        size_t spanCount = region->LargeSpanCount();
        largeRegionList_.Remove(region);
        largeLiveSpanCount_.fetch_sub(spanCount, std::memory_order_release);
        largeLiveObjectBytes_.fetch_sub(region->LargeObjectSize(), std::memory_order_release);
        deadRegions.push_back(region);
    }
}

// ============================================================================
// Large object hot cache
// ============================================================================

bool CentralRegionPool::TryPutLargeHotCache(uintptr_t regionStart, size_t regionBytes)
{
    size_t regionCount = regionBytes / MS_REGION_SIZE;
    if (regionCount == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lg(largeHotCacheLock_);
    largeHotCache_[regionCount].push_back({regionStart, regionBytes});
    largeHotCacheRegionCount_.fetch_add(regionCount, std::memory_order_release);
    largeHotCacheBytes_.fetch_add(regionBytes, std::memory_order_release);
    return true;
}

bool CentralRegionPool::TryTakeLargeHotCache(size_t regionBytes, uintptr_t& regionStart)
{
    size_t regionCount = regionBytes / MS_REGION_SIZE;
    if (regionCount == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lg(largeHotCacheLock_);
    auto it = largeHotCache_.find(regionCount);
    if (it == largeHotCache_.end() || it->second.empty()) {
        return false;
    }
    auto& bucket = it->second;
    regionStart = bucket.back().regionStart;
    bucket.pop_back();
    if (bucket.empty()) {
        largeHotCache_.erase(it);
    }
    largeHotCacheRegionCount_.fetch_sub(regionCount, std::memory_order_release);
    largeHotCacheBytes_.fetch_sub(regionBytes, std::memory_order_release);
    return true;
}

size_t CentralRegionPool::ReclaimLargeHotCache(size_t maxBytes)
{
    if (maxBytes == 0) {
        return 0;
    }
    std::vector<LargeHotEntry> toReclaim;
    size_t flushed = 0;
    size_t reclaimedRegions = 0;
    {
        std::lock_guard<std::mutex> lg(largeHotCacheLock_);
        // Reclaim largest objects first: higher RSS impact, lower reuse probability.
        for (auto it = largeHotCache_.rbegin(); it != largeHotCache_.rend() && flushed < maxBytes; ++it) {
            size_t key = it->first;
            if (flushed >= maxBytes) {
                break;
            }
            auto& bucket = it->second;
            while (!bucket.empty() && flushed < maxBytes) {
                LargeHotEntry entry = bucket.back();
                bucket.pop_back();
                toReclaim.push_back(entry);
                flushed += entry.regionBytes;
                reclaimedRegions += key;
            }
        }

        for (auto it = largeHotCache_.begin(); it != largeHotCache_.end();) {
            if (it->second.empty()) {
                it = largeHotCache_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (flushed != 0) {
        largeHotCacheRegionCount_.fetch_sub(reclaimedRegions, std::memory_order_release);
        largeHotCacheBytes_.fetch_sub(flushed, std::memory_order_release);
    }
    std::vector<FreeManager::RegionSpan> spans;
    spans.reserve(toReclaim.size());
    for (auto& entry : toReclaim) {
        auto regionIdx = static_cast<FreeManager::RegionIndex>(
            (entry.regionStart - msRegionManager_->GetHeapStart()) / MS_REGION_SIZE);
        auto regionCount = static_cast<FreeManager::RegionCount>(entry.regionBytes / MS_REGION_SIZE);
        spans.push_back({ regionIdx, regionCount });
    }
    msRegionManager_->ReclaimReleasedRegionsToHotFreeBatch(spans);
    return flushed;
}

void CentralRegionPool::SweepLargeRegions()
{
    // Flush unconsumed hot-cache entries from the previous GC cycle first.
    (void)ReclaimLargeHotCache(std::numeric_limits<size_t>::max());

    // Two-pass to avoid holding largeLock_ during ReclaimRegion.
    std::vector<MSRegionInfo*> deadRegions;

    CollectDeadLargeRegions(deadRegions);

    // Release dead large regions
    for (MSRegionInfo* msInfo : deadRegions) {
        uintptr_t regionStart = msInfo->LargeObjectStart();
        size_t regionBytes = msInfo->LargeSpanBytes();
        size_t spanCount = msInfo->LargeSpanCount();

        // Reset span MSRegionInfo entries before resetting the base entry.
        // RegisterLargeObjectMS initializes span entries with regionType=LARGE
        // and reserved=baseIdx; we must clear them to avoid stale lookups.
        size_t baseIdx = MSRegionInfoArray::GetRegionIdx(msInfo);
        for (size_t i = 1; i < spanCount; i++) {
            MSRegionInfo* spanInfo = MSRegionInfoArray::GetRegionInfoByIdx(baseIdx + i);
            spanInfo->Reset();
        }

        // Reset base MSRegionInfo — leaves regionType=MS_REGION_FREE which
        // RegisterLargeObjectMS accepts when this entry is reused from hot cache.
        msInfo->Reset();

        // Try to keep physical pages hot for fast reuse; fall back to normal reclaim.
        if (!TryPutLargeHotCache(regionStart, regionBytes)) {
            msRegionManager_->ReclaimRegionLarge(regionStart, regionBytes);
        }
    }
}

// ============================================================================
// Per-thread AllocBufferMS registry
// ============================================================================
void CentralRegionPool::RegisterAllocBufferMS(AllocBufferMS* buffer)
{
    std::lock_guard<std::mutex> lg(msAllocBufferLock_);
    msAllocBuffers_.push_back(buffer);
}

void CentralRegionPool::RemoveAllocBufferMS(AllocBufferMS* buffer)
{
    std::lock_guard<std::mutex> lg(msAllocBufferLock_);
    auto it = std::find(msAllocBuffers_.begin(), msAllocBuffers_.end(), buffer);
    if (it != msAllocBuffers_.end()) {
        msAllocBuffers_.erase(it);
    }
}

// ============================================================================
// ComputeMarkedStats: walk markBitmaps once for object count and live bytes
// ============================================================================
// Called after marking and large eager reclaim complete, before
// PostMarkTermination publishes small-Region Sweep work.  At this point
// markBitmaps are stable and no small-Region sweep has started.
//
MSMarkedStats CentralRegionPool::ComputeMarkedStats() const
{
    MSMarkedStats stats;

    // 1. Walk all small regions in CentralPool (all 4 lists × 36 classes)
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        const SizeClassBucket& bucket = buckets_[cls];
        size_t slotSize = SizeClassInfo::ClassToSize(cls);

        auto countRegion = [slotSize, &stats](MSRegionInfo* region) {
            if (region->markBitmap == nullptr || region->totalSlotCount == 0) {
                return;
            }
            size_t wordCount = (region->totalSlotCount + 63) / 64;
            size_t marked = 0;
            for (size_t i = 0; i < wordCount; i++) {
                marked += __builtin_popcountll(region->markBitmap[i]);
            }
            stats.AddSlots(marked, slotSize);
        };

        bucket.VisitPartialSweptUnsafe(countRegion);
        bucket.VisitLockedLists(countRegion, countRegion, countRegion);
    }

    // 2. Walk thread-local AllocBufferMS active regions (not in central pool)
    {
        std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
        for (AllocBufferMS* buffer : msAllocBuffers_) {
            if (buffer == nullptr) {
                continue;
            }
            for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
                MSRegionInfo* region = buffer->GetActiveRegion(cls);
                if (region == nullptr || region->markBitmap == nullptr ||
                    region->totalSlotCount == 0) {
                    continue;
                }
                size_t slotSize = SizeClassInfo::ClassToSize(cls);
                size_t wordCount = (region->totalSlotCount + 63) / 64;
                size_t marked = 0;
                for (size_t i = 0; i < wordCount; i++) {
                    marked += __builtin_popcountll(region->markBitmap[i]);
                }
                stats.AddSlots(marked, slotSize);
            }
        }
    }

    // 3. Live large regions (isMarked == 1 after CollectLargeGarbage)
    {
        VisitLargeRegionsLocked([&stats](MSRegionInfo* region) {
            if (region->IsMarked()) {
                stats.AddLargeObject(region->LargeSpanBytes());
            }
        });
    }

    return stats;
}

// ============================================================================
// VisitMarkedObjects: walk markBitmaps and call func() on every marked object.
// ============================================================================
// Same validity window as ComputeMarkedStats: marking complete, no sweep
// started. Used by the mark-completeness verifier to inspect every live object.
//
void CentralRegionPool::VisitMarkedObjects(const std::function<void(BaseObject*)>& func) const
{
    // Walk one region's markBitmap, emitting each marked slot as a BaseObject*.
    auto visitRegion = [&func](MSRegionInfo* region, size_t slotSize) {
        if (region->markBitmap == nullptr || region->totalSlotCount == 0) {
            return;
        }
        uintptr_t base = region->RegionStart();
        size_t wordCount = (region->totalSlotCount + 63) / 64;
        for (size_t w = 0; w < wordCount; w++) {
            uint64_t bits = region->markBitmap[w];
            while (bits != 0) {
                size_t bit = static_cast<size_t>(__builtin_ctzll(bits));
                bits &= (bits - 1);
                size_t slotIdx = w * 64 + bit;
                uintptr_t slotAddr = base + slotIdx * slotSize;
                func(reinterpret_cast<BaseObject*>(slotAddr));
            }
        }
    };

    // 1. All small regions across all 4 lists × size classes.
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        const SizeClassBucket& bucket = buckets_[cls];
        size_t slotSize = SizeClassInfo::ClassToSize(cls);
        auto countRegion = [&visitRegion, slotSize](MSRegionInfo* region) {
            visitRegion(region, slotSize);
        };
        bucket.VisitPartialSweptUnsafe(countRegion);
        bucket.VisitLockedLists(countRegion, countRegion, countRegion);
    }

    // 2. Thread-local AllocBufferMS active regions.
    {
        std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
        for (AllocBufferMS* buffer : msAllocBuffers_) {
            if (buffer == nullptr) {
                continue;
            }
            for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
                MSRegionInfo* region = buffer->GetActiveRegion(cls);
                if (region == nullptr) {
                    continue;
                }
                visitRegion(region, SizeClassInfo::ClassToSize(cls));
            }
        }
    }

    // 3. Live large regions.
    {
        VisitLargeRegionsLocked([&func](MSRegionInfo* region) {
            if (region->IsMarked()) {
                func(reinterpret_cast<BaseObject*>(region->LargeObjectStart()));
            }
        });
    }
}

// ============================================================================
// Pre-STW MarkSetup: Stop all sweeps, clear markBitmaps, reclassify lists
// ============================================================================

void CentralRegionPool::StopAndWaitForAllSweep()
{
    // The GC main thread participates as an uncounted Sweep claimant below.
    // Keep completion closed across both drains and the intervening quiescence
    // waits so another executor cannot publish done after GC main claims the
    // final Region but before it finishes SweepRegion().
    forceFinishSweepRunning_.store(true, std::memory_order_release);
    size_t unsweptBefore = GetTotalUnsweptCount();

    // 1. Join the still-open Sweep epoch as another claimant. Every Sweep
    // source uses ClaimUnsweptHeadForSweep(), so the GC thread can drain
    // different Regions concurrently with background/lazy/assist/helper work.
    // This pass deliberately does not publish sweepDone_: claimed Regions may
    // still be inside SweepRegion on another thread.
    ForceFinishAllUnswept();

    // 2. Close all Sweep admission paths before establishing quiescence. The
    // unified admission word makes closing and active-count accounting one
    // atomic protocol; these flags remain as lane-specific fast exits.
    stopSweepRequested_.store(true, std::memory_order_release);
    skipLazySweep_.store(true, std::memory_order_release);
    skipAssistSweep_.store(true, std::memory_order_release);
    skipHelperSweep_.store(true, std::memory_order_release);

    // 3. Wait for every admitted sweep source. A worker that starts after this
    // close cannot increment the packed active count.
    CloseSweepAdmissionAndWait();

    // 4. Drain anything left unclaimed while admission was being
    // closed. With every in-flight source quiescent, this is the final serial
    // closure before mark bitmap clearing.
    ForceFinishAllUnswept();
    CHECK(GetTotalUnsweptCount() == 0);
    CHECK(GetTotalUnsweptCountReference() == 0);
    forceFinishSweepRunning_.store(false, std::memory_order_release);
    PublishSweepDoneAfterQuiescence();
    CHECK(IsSweepDone());

    size_t unsweptAfter = GetTotalUnsweptCount();
    size_t activeBytes = GetActiveMSRegionCount() * MS_REGION_SIZE;
    size_t emptySwept = GetEmptySweptRegionCount() * MS_REGION_SIZE;
    size_t largeCache = GetLargeHotCacheRegionCount() * MS_REGION_SIZE;
    size_t totalRetained = activeBytes + emptySwept + largeCache;
    VLOG(REPORT, "[MS-GC] StopAndWaitForAllSweep: forceSwept=%zu->%zu unswept regions, "
         "retained=%zuB activeBytes=%zuB emptySwept=%zuB largeCache=%zuB",
         unsweptBefore, unsweptAfter,
         totalRetained, activeBytes, emptySwept, largeCache);
}

void CentralRegionPool::ClearAllMarkBitmaps()
{
    size_t maxRegions = msRegionManager_->GetMaxRegionCount();
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);

    size_t clearedCount = 0;
    size_t largeCleared = 0;

    for (size_t i = 0; i < maxRegions; i++) {
        MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoByIdx(i);

        MSRegionType type = info->regionType;

        // Skip free/uninitialized/empty-cache/span entries
        if (type == MSRegionType::MS_REGION_FREE ||
            type == MSRegionType::MS_REGION_EMPTY_CACHE ||
            type == MSRegionType::MS_REGION_SPAN) {
            continue;
        }

        // Large regions: clear isMarked (no markBitmap to clear).
        // Replaces the separate ClearLargeMarks() + largeLock_ traversal.
        if (type == MSRegionType::MS_REGION_LARGE) {
            if (info->GetSweepGen() != sg) {
                info->SetSweepGen(sg);
            }
            info->ClearMarkedFlag();
            largeCleared++;
            continue;
        }

        // Skip regions with no bitmap (not yet initialized)
        if (info->regionEnd == 0) {
            continue;
        }

        (void)info->CasSweepGen(sg - 2, sg);

        // Clear markBitmap unconditionally for all active small/medium regions.
        // Safe because: no SweepRegion is running (StopAndWaitForAllSweep),
        // and mark-at-alloc is inactive (GC phase is IDLE/SWEEP).
        info->ClearMarkBitmap();
        clearedCount++;
    }

    VLOG(REPORT,
         "[MS-GC] ClearAllMarkBitmaps: clearedSmall=%zu regions, clearedLarge=%zu regions, committedRegions=%zu",
         clearedCount, largeCleared, maxRegions);
}

void CentralRegionPool::ReclassifyUnsweptToSwept()
{
    size_t partialMoved = 0;
    size_t fullMoved = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        buckets_[cls].ReclassifyUnsweptToSwept(partialMoved, fullMoved);
    }
    centralSmallUnsweptRegions_.store(0, std::memory_order_release);

    VLOG(REPORT,
         "[MS-GC] ReclassifyUnsweptToSwept: partial=%zu regions, full=%zu regions, total=%zu",
         partialMoved, fullMoved, partialMoved + fullMoved);
}

void CentralRegionPool::ReenableLazySweep()
{
    skipLazySweep_.store(false, std::memory_order_release);
    skipAssistSweep_.store(false, std::memory_order_release);
    skipHelperSweep_.store(false, std::memory_order_release);
    stopSweepRequested_.store(false, std::memory_order_release);
    OpenSweepAdmission();
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
