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
#include "Concurrency/ConcurrencyModel.h"
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
uint32_t GetProcessorCountForAssistSweep()
{
    if (Runtime::CurrentRef() == nullptr) {
        return 1;
    }
    uint32_t processorNum = Runtime::Current().GetConcurrencyModel().GetProcessorNum();
    return processorNum == 0 ? 1 : processorNum;
}

// 后台批量处理超时时间。budgetMultiplier 由 sweep-debt tier 决定（NORMAL 1x /
// BACKLOG 4x / EMERGENCY 10x），用于在 backlog 偏大时放大单次后台 sweep 时间片。
uint64_t GetBgSweepMaxTimeUs(double budgetMultiplier)
{
    uint64_t valueNs = CangjieRuntime::GetGCParam().msBackgroundSweepMaxTime;
    uint64_t baseValue = valueNs == 0 ? 0 : std::max<uint64_t>(1, valueNs / 1000ULL);
    if (budgetMultiplier <= 1.0) {
        return baseValue;
    }
    double scaled = static_cast<double>(baseValue) * budgetMultiplier;
    double maxVal = static_cast<double>(std::numeric_limits<uint64_t>::max());
    return scaled >= maxVal ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(scaled);
}

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
{}

void CentralRegionPool::ResetSweepCounts()
{
    backgroundSweptRegions_.store(0, std::memory_order_relaxed);
    lazySweptRegions_.store(0, std::memory_order_relaxed);
    assistSweptRegions_.store(0, std::memory_order_relaxed);
    helperSweptRegions_.store(0, std::memory_order_relaxed);
}

MSSweepCounts CentralRegionPool::GetSweepCounts() const
{
    MSSweepCounts counts;
    counts.backgroundSweptRegions = backgroundSweptRegions_.load(std::memory_order_acquire);
    counts.lazySweptRegions = lazySweptRegions_.load(std::memory_order_acquire);
    counts.assistSweptRegions = assistSweptRegions_.load(std::memory_order_acquire);
    counts.helperSweptRegions = helperSweptRegions_.load(std::memory_order_acquire);
    return counts;
}

bool CentralRegionPool::TryPublishSweepDone()
{
    if (GetTotalUnsweptCount() != 0 ||
        bgSweepRunning_.load(std::memory_order_acquire) ||
        activeLazySweepCount_.load(std::memory_order_acquire) != 0 ||
        activeAssistSweepCount_.load(std::memory_order_acquire) != 0 ||
        activeHelperSweepCount_.load(std::memory_order_acquire) != 0) {
        return false;
    }
    sweepDone_.store(true, std::memory_order_release);
    return true;
}

void CentralRegionPool::MaybeNotifySweep(uint8_t cls) const
{
    if (msRegionManager_ != nullptr) {
        msRegionManager_->MaybeNotifySweep(this, cls);
    }
}

std::array<uint8_t, kNumSizeClasses> CentralRegionPool::BuildBackgroundSweepClassOrder(uint8_t preferredCls) const
{
    std::array<uint8_t, kNumSizeClasses> order {};
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        order[cls] = cls;
    }

    std::sort(order.begin(), order.end(), [this, preferredCls](uint8_t lhs, uint8_t rhs) {
        bool lhsPreferred = preferredCls < kNumSizeClasses && lhs == preferredCls;
        bool rhsPreferred = preferredCls < kNumSizeClasses && rhs == preferredCls;
        if (lhsPreferred != rhsPreferred) {
            return lhsPreferred;
        }
        BucketPressure lhsPressure = GetBucketPressure(lhs);
        BucketPressure rhsPressure = GetBucketPressure(rhs);
        bool lhsHasUnswept = lhsPressure.UnsweptCount() > 0;
        bool rhsHasUnswept = rhsPressure.UnsweptCount() > 0;
        if (lhsHasUnswept != rhsHasUnswept) {
            return lhsHasUnswept;
        }
        if (lhsPressure.ReadyCount() != rhsPressure.ReadyCount()) {
            return lhsPressure.ReadyCount() < rhsPressure.ReadyCount();
        }
        if (lhsPressure.UnsweptCount() != rhsPressure.UnsweptCount()) {
            return lhsPressure.UnsweptCount() > rhsPressure.UnsweptCount();
        }
        return lhs < rhs;
    });
    return order;
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

    if (skipLazySweep_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    // Notify background sweep only when we actually enter unswept paths,
    // rather than speculatively at central acquisition entry.
    msRegionManager_->MaybeNotifySweep(this, cls);
    activeLazySweepCount_.fetch_add(1, std::memory_order_relaxed);

    // Double-check after increment to close the race with StopAndWaitForAllSweep.
    // The increment (a store to activeLazySweepCount_) and this skip re-load form
    // a StoreLoad pair against StopAndWaitForAllSweep's skip-store / count-load.
    // acquire/release alone does NOT order StoreLoad across two different atomics,
    // so without this seq_cst fence the increment can sink below the skip load:
    // Stop then observes count==0, runs ClearAllMarkBitmaps concurrently with this
    // thread's SweepRegion, and ~markBitmap frees LIVE objects (corrupting e.g. the
    // JSON parse buffer). The matching fence lives in StopAndWaitForAllSweep.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (skipLazySweep_.load(std::memory_order_acquire)) {
        activeLazySweepCount_.fetch_sub(1, std::memory_order_release);
        (void)TryPublishSweepDone();
        return nullptr;
    }

    MSRegionInfo* sweptRegion = SweepUnsweptList(UnsweptListKind::PARTIAL);
    if (sweptRegion == nullptr) {
        sweptRegion = SweepUnsweptList(UnsweptListKind::FULL);
    }
    activeLazySweepCount_.fetch_sub(1, std::memory_order_release);
    (void)TryPublishSweepDone();
    return sweptRegion;
}

// ============================================================================
// GetFreshRegion — allocate a brand-new Region from MSRegionManager
// ============================================================================
MSRegionInfo* CentralRegionPool::GetFreshRegion(uint8_t cls, uint16_t shardHint)
{
    // MSRegionManager::TakeRegion allocates one MS_REGION_SIZE region
    // and returns the corresponding MSRegionInfo* directly (one-level).
    MSRegionInfo* msInfo = msRegionManager_->TakeRegion(cls, *this, shardHint);
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
    for (const auto& [regionCount, bucket] : largeHotCache_) {
        (void)regionCount;
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
    ResetSweepCounts();
    size_t totalUnswept = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        buckets_[cls].FlipSweptToUnswept();
        totalUnswept += buckets_[cls].CountPartialUnswept() + buckets_[cls].CountFullUnswept();
    }

    // Reset the per-cycle central credit and publish the fixed-point proportional
    // Sweep ratio once. Allocation progress and debt live in each AllocBufferMS.
    // runway = how much more allocation can happen before the next GC trigger,
    // measured on the swept-region-bytes axis the HEU trigger actually compares
    // against (threshold vs GetSweptHeapBytes), consistent with
    // BackgroundProcessor::HasPendingSweepWork. At cycle start sweptBytes is ~0 so
    // runway ~= threshold. threshold is stable within a cycle (only changed at GC
    // boundaries), so the ratio is fixed here and read without re-computation.
    sweepPressureMode_.store(static_cast<int>(SweepPressureMode::NORMAL), std::memory_order_relaxed);
    sweepCredit_.Reset();
    {
        std::lock_guard<std::mutex> lg(msAllocBufferLock_);
        exitedSweepAssistStats_ = MSSweepAssistStats {};
    }
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
// BackgroundSweep — called by gc-helper thread (FinalizerProcessor)
// ============================================================================

BackgroundSweepStats CentralRegionPool::BackgroundSweep(uint8_t preferredCls)
{
    bgSweepRunning_.store(true, std::memory_order_release);
    auto finish = [this]() {
        bgSweepRunning_.store(false, std::memory_order_release);
    };

    // seq_cst fence: bgSweepRunning_ store + stopSweepRequested_ load form a
    // StoreLoad pair against StopAndWaitForAllSweep (which stores stopSweepRequested_
    // then loads bgSweepRunning_). Without it the background SweepRegion could run
    // concurrently with ClearAllMarkBitmaps and free live objects.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    if (stopSweepRequested_.load(std::memory_order_acquire)) {
        finish();
        return {};
    }

    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    uint64_t t0 = TimeUtil::MicroSeconds();
    size_t remainingUnswept = GetTotalUnsweptCount();
    if (remainingUnswept == 0) {
        finish();
        (void)TryPublishSweepDone();
        return {};
    }
    // Per-slice time budget scales with the latched sweep-debt tier (Step34 §18.4):
    // NORMAL 1x, BACKLOG 4x, EMERGENCY 10x, so a large backlog gets drained
    // allocation-independently before the next GC instead of via ForceFinishAllUnswept.
    uint64_t deadlineUs = t0 + GetBgSweepMaxTimeUs(SweepTierMultiplier(GetSweepPressureMode()));
    size_t sweptInSlice = 0;
    size_t emptyInSlice = 0;
    bool pendingWork = false;

    auto SweepBucket = [&](uint8_t cls) -> bool {
        if (stopSweepRequested_.load(std::memory_order_acquire)) {
            if (sweptInSlice > 0) {
                VLOG(REPORT, "[MS-GC] BackgroundSweep: stop requested at cls=%u", (unsigned)cls);
            }
            pendingWork = false;
            return false;
        }

        SizeClassBucket& bucket = buckets_[cls];
        if (!SweepUnsweptList(bucket, SizeClassBucket::UnsweptListKind::PARTIAL,
                              sg, sweptInSlice, SIZE_MAX, deadlineUs, &emptyInSlice)) {
            pendingWork = !stopSweepRequested_.load(std::memory_order_acquire);
            return false;
        }
        if (!SweepUnsweptList(bucket, SizeClassBucket::UnsweptListKind::FULL,
                              sg, sweptInSlice, SIZE_MAX, deadlineUs, &emptyInSlice)) {
            pendingWork = !stopSweepRequested_.load(std::memory_order_acquire);
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

    uint64_t elapsed = TimeUtil::MicroSeconds() - t0;

    RecordBackgroundSweep(sweptInSlice);
    finish();
    if (!pendingWork && !stopSweepRequested_.load(std::memory_order_acquire)) {
        (void)TryPublishSweepDone();
    }
    return BackgroundSweepStats{1, sweptInSlice, emptyInSlice, elapsed};
}

BackgroundSweepStats CentralRegionPool::ParallelSweep(size_t maxRegions, uint8_t preferredCls,
                                                       uint64_t deadlineUs)
{
    if (maxRegions == 0) {
        return {};
    }

    activeHelperSweepCount_.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (skipHelperSweep_.load(std::memory_order_acquire) ||
        stopSweepRequested_.load(std::memory_order_acquire)) {
        activeHelperSweepCount_.fetch_sub(1, std::memory_order_release);
        (void)TryPublishSweepDone();
        return {};
    }

    uint64_t t0 = TimeUtil::MicroSeconds();
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    size_t sweptInSlice = 0;
    size_t emptyInSlice = 0;
    auto classOrder = BuildBackgroundSweepClassOrder(preferredCls);
    for (uint8_t cls : classOrder) {
        SizeClassBucket& bucket = buckets_[cls];
        if (!SweepUnsweptList(bucket, SizeClassBucket::UnsweptListKind::PARTIAL,
                              sg, sweptInSlice, maxRegions, deadlineUs, &emptyInSlice)) {
            break;
        }
        if (!SweepUnsweptList(bucket, SizeClassBucket::UnsweptListKind::FULL,
                              sg, sweptInSlice, maxRegions, deadlineUs, &emptyInSlice)) {
            break;
        }
    }

    RecordHelperSweep(sweptInSlice);
    uint64_t elapsed = TimeUtil::MicroSeconds() - t0;
    activeHelperSweepCount_.fetch_sub(1, std::memory_order_release);
    (void)TryPublishSweepDone();
    return BackgroundSweepStats{1, sweptInSlice, emptyInSlice, elapsed};
}

size_t CentralRegionPool::ComputeAssistSweepRegions(size_t unsweptRegions, uint32_t processorNum,
                                                    bool hardPressure) const
{
    size_t normalMaxAssistRegions = CangjieRuntime::GetGCParam().msAssistSweepMaxRegionsPerCall;

    if (unsweptRegions == 0) {
        return 0;
    }
    size_t safeProcessorNum = std::max<size_t>(1, static_cast<size_t>(processorNum));
    if (hardPressure) {
        size_t assistRegions = unsweptRegions / safeProcessorNum;
        size_t hardMaxAssistRegions = normalMaxAssistRegions >= std::numeric_limits<size_t>::max() / 10U
            ? std::numeric_limits<size_t>::max()
            : normalMaxAssistRegions * 10U;
        return std::min(assistRegions, hardMaxAssistRegions);
    }

    size_t assistRegions = unsweptRegions / safeProcessorNum / 10U;
    return std::min(assistRegions, normalMaxAssistRegions);
}

size_t CentralRegionPool::AssistSweepForFreshRegions(size_t requestedRegions, uint8_t preferredCls,
                                                     AllocBufferMS* preferredOwner)
{
    if (requestedRegions == 0) {
        return 0;
    }

    size_t unsweptRegions = GetTotalUnsweptCount();
    if (unsweptRegions == 0) {
        return 0;
    }
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    size_t thresholdBytes = space.GetHeapThresholdBytes();
    size_t sweptBytes = space.GetSweptHeapBytes();
    size_t remainingRunwayBytes = thresholdBytes > sweptBytes ? (thresholdBytes - sweptBytes) : 0;
    bool hardPressure = remainingRunwayBytes == 0;
    if (!hardPressure) {
        size_t remainingRunwayRegions = (remainingRunwayBytes + MS_REGION_SIZE - 1) / MS_REGION_SIZE;
        if (unsweptRegions <= remainingRunwayRegions) {
            return 0;
        }
    }

    uint32_t processorNum = GetProcessorCountForAssistSweep();
    size_t assistRegions = ComputeAssistSweepRegions(unsweptRegions, processorNum, hardPressure);
    if (assistRegions == 0) {
        return 0;
    }
    return AssistSweep(assistRegions, preferredCls, preferredOwner);
}

MSSweepAssistResult CentralRegionPool::MSSweepAssist(AllocBufferMS& owner, size_t debtRegions,
                                                     uint8_t preferredCls)
{
    MSSweepAssistResult result;
    if (debtRegions == 0 || sweepDone_.load(std::memory_order_acquire)) {
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

    // Bound direct repayment per allocation call; fractional and unpaid debt
    // stays on the owner P for its next successful Region consumption.
    size_t baseMaxPerCall = CangjieRuntime::GetGCParam().msAssistSweepMaxRegionsPerCall;
    uint64_t scaledMax = ScaleMSSweepAssistRatioScaled(
        static_cast<uint64_t>(baseMaxPerCall), GetSweepPressureMode());
    size_t maxPerCall = scaledMax > std::numeric_limits<size_t>::max()
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(scaledMax);
    size_t toSweep = std::min(remainingDebt, std::max<size_t>(maxPerCall, 1));

    // AssistSweep carries the activeAssistSweepCount_ guard + seq_cst fence + skip
    // checks that pair with StopAndWaitForAllSweep, so STW correctness is preserved.
    result.assistPaidRegions = AssistSweep(toSweep, preferredCls, &owner);
    result.sweepComplete = sweepDone_.load(std::memory_order_acquire);
    return result;
}

SweepDebtDecision CentralRegionPool::RefreshSweepDebtSnapshot(size_t thresholdBytes, size_t sweptBytes)
{
    SweepDebtParams params;  // defaults: backlog 2.0/1.0, emergency 64/16
    size_t unsweptRegions = GetTotalUnsweptCount();
    size_t runwayRegions = ComputeRunwayRegions(thresholdBytes, sweptBytes, MS_REGION_SIZE);
    bool sweepDone = IsSweepDone();

    SweepPressureMode prev = GetSweepPressureMode();
    SweepPressureMode mode =
        EvaluateSweepPressure(unsweptRegions, runwayRegions, sweepDone, prev, params);
    sweepPressureMode_.store(static_cast<int>(mode), std::memory_order_relaxed);

    return BuildSweepDebtDecision(mode, sweepDone);
}

size_t CentralRegionPool::AssistSweep(size_t targetRegions, uint8_t preferredCls, AllocBufferMS* preferredOwner)
{
    if (targetRegions == 0) {
        return 0;
    }

    activeAssistSweepCount_.fetch_add(1, std::memory_order_relaxed);

    // Pair with StopAndWaitForAllSweep's seq_cst fence so the assist-skip store
    // and activeAssistSweepCount_ load form a sequentially consistent StoreLoad.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (skipAssistSweep_.load(std::memory_order_acquire) ||
        stopSweepRequested_.load(std::memory_order_acquire)) {
        activeAssistSweepCount_.fetch_sub(1, std::memory_order_release);
        (void)TryPublishSweepDone();
        return 0;
    }

    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    size_t sweptRegions = 0;
    auto classOrder = BuildBackgroundSweepClassOrder(preferredCls);
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
    (void)TryPublishSweepDone();
    return sweptRegions;
}

bool CentralRegionPool::SweepOneUnsweptRegionForAssist(SizeClassBucket& bucket,
                                                       SizeClassBucket::UnsweptListKind listKind,
                                                       uint32_t sg,
                                                       AllocBufferMS* preferredOwner,
                                                       size_t* emptiedInSlice)
{
    for (;;) {
        if (stopSweepRequested_.load(std::memory_order_acquire)) {
            return false;
        }

        MSRegionInfo* region = bucket.ClaimUnsweptHeadForSweep(listKind, sg);
        if (region == nullptr) {
            return false;
        }

        region->SweepRegion();
        region->SetSweepGen(sg);
        RecordAssistSweep(1);
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
        // Check stop request per-region for prompt exit
        if (stopSweepRequested_.load(std::memory_order_acquire)) {
            return false;
        }

        if (sweptInSlice >= maxRegions || TimeUtil::MicroSeconds() >= deadlineUs) {
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

        if ((sweptInSlice & 0x7U) == 0) {
            std::this_thread::yield();
        }
    }

    return true;
}

// ===== Monitoring =====

size_t CentralRegionPool::GetTotalUnsweptCount() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        total += buckets_[cls].CountPartialUnswept();
        total += buckets_[cls].CountFullUnswept();
    }
    return total;
}

size_t CentralRegionPool::GetLargeRegionCount() const
{
    return largeRegionList_.Count();
}

void CentralRegionPool::ForceFinishUnsweptList(SizeClassBucket& bucket, SizeClassBucket::UnsweptListKind listKind,
                                            uint8_t cls, uint32_t sg,
                                            size_t& sweptRegions, size_t& emptiedRegions,
                                            size_t& retainedRegions, size_t& freedBytes)
{
    (void)cls;
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
        ForceFinishUnsweptList(bucket, SizeClassBucket::UnsweptListKind::PARTIAL, cls, sg,
                            sweptRegions, emptiedRegions, retainedRegions, freedBytes);
        ForceFinishUnsweptList(bucket, SizeClassBucket::UnsweptListKind::FULL, cls, sg,
                            sweptRegions, emptiedRegions, retainedRegions, freedBytes);
    }

    sweepDone_.store(true, std::memory_order_release);

    VLOG(REPORT,
        "[MS-GC] ForceFinishAllUnswept: swept=%zu regions, emptied=%zu, retained=%zu, freed=%s B",
        sweptRegions, emptiedRegions, retainedRegions, Pretty(freedBytes).Str());
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

size_t CentralRegionPool::GetAllAllocBufferCachedRegionCount() const
{
    size_t total = 0;
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
    for (AllocBufferMS* buffer : msAllocBuffers_) {
        if (buffer != nullptr) {
            total += buffer->CountCachedRegions();
        }
    }
    return total;
}

bool CentralRegionPool::HasHungryAllocBuffer() const
{
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
    for (AllocBufferMS* buffer : msAllocBuffers_) {
        if (buffer != nullptr && buffer->NeedsBackgroundFill()) {
            return true;
        }
    }
    return false;
}

size_t CentralRegionPool::GetAllAllocBufferRegionCount() const
{
    return GetAllAllocBufferActiveRegionCount() + GetAllAllocBufferCachedRegionCount();
}

size_t CentralRegionPool::GetSweptMSRegionCount() const
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
    size_t spanCount = (region->regionEnd - region->RegionStart()) / MS_REGION_SIZE;
    MSRegionInfoArray::SetState(region, MSRegionLocation::LARGE_ACTIVE, 0);
    std::lock_guard<std::mutex> lg(largeLock_);
    largeRegionList_.Prepend(region);
    largeLiveSpanCount_.fetch_add(spanCount, std::memory_order_release);
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
        size_t spanCount = (region->regionEnd - region->RegionStart()) / MS_REGION_SIZE;
        largeRegionList_.Remove(region);
        largeLiveSpanCount_.fetch_sub(spanCount, std::memory_order_release);
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

MSLargeSweepStats CentralRegionPool::SweepLargeRegions()
{
    // Flush unconsumed hot-cache entries from the previous GC cycle first.
    (void)ReclaimLargeHotCache(std::numeric_limits<size_t>::max());

    // Two-pass to avoid holding largeLock_ during ReclaimRegion.
    std::vector<MSRegionInfo*> deadRegions;
    MSLargeSweepStats stats;

    CollectDeadLargeRegions(deadRegions);

    // Release dead large regions
    for (MSRegionInfo* msInfo : deadRegions) {
        uintptr_t regionStart = msInfo->allocPtr;  // For LARGE, allocPtr = start
        size_t regionBytes = msInfo->regionEnd - regionStart;
        size_t spanCount = (regionBytes + MS_REGION_SIZE - 1) / MS_REGION_SIZE;
        stats.objectCount++;
        stats.regionCount += spanCount;
        stats.freedBytes += regionBytes;

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
        if (TryPutLargeHotCache(regionStart, regionBytes)) {
            stats.cachedObjectCount++;
            stats.cachedRegionCount += spanCount;
            stats.cachedBytes += regionBytes;
        } else {
            msRegionManager_->ReclaimRegionLarge(regionStart, regionBytes);
        }
    }

    return stats;
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
        uint32_t currentGeneration = globalSweepGen_.load(std::memory_order_acquire);
        if (buffer->GetSweepDebtGeneration() == currentGeneration) {
            exitedSweepAssistStats_.Merge(buffer->SnapshotSweepAssistStats());
        }
        msAllocBuffers_.erase(it);
    }
}

MSSweepAssistStats CentralRegionPool::SnapshotSweepAssistStats()
{
    std::lock_guard<std::mutex> lg(msAllocBufferLock_);
    MSSweepAssistStats total = exitedSweepAssistStats_;
    uint32_t currentGeneration = globalSweepGen_.load(std::memory_order_acquire);
    for (AllocBufferMS* buffer : msAllocBuffers_) {
        if (buffer == nullptr || buffer->GetSweepDebtGeneration() != currentGeneration) {
            continue;
        }
        MSSweepAssistStats snapshot = buffer->SnapshotSweepAssistStats();
        // A P publishes its new generation only after resetting its statistics.
        // Recheck so a concurrent lazy reset cannot mix adjacent cycles.
        if (buffer->GetSweepDebtGeneration() == currentGeneration) {
            total.Merge(snapshot);
        }
    }
    return total;
}

BackgroundFeedStats CentralRegionPool::BackgroundFillAllocCaches()
{
    BackgroundFeedStats totalStats;
    totalStats.slices = 1;

    std::vector<AllocBufferMS*> buffers;
    {
        std::lock_guard<std::mutex> lg(msAllocBufferLock_);
        buffers = msAllocBuffers_;
    }
    totalStats.buffersScanned = buffers.size();
    uint64_t t0 = TimeUtil::MicroSeconds();
    for (AllocBufferMS* buffer : buffers) {
        if (buffer != nullptr) {
            MSAllocCacheFillStats stats = buffer->BackgroundFillCache();
            if (stats.HasWork()) {
                totalStats.touchedBuffers++;
                totalStats.filledClasses += stats.filledClasses;
                totalStats.filledRegions += stats.filledRegions;
            }
        }
    }
    totalStats.elapsedUs = TimeUtil::MicroSeconds() - t0;
    return totalStats;
}

// ============================================================================
// ComputeMarkedStats: walk markBitmaps once for object count and live bytes
// ============================================================================
// Called after marking completes, before PrepareForSweep (gen flip).
// At this point markBitmaps are stable: marking is done, no sweep has started.
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
                stats.AddLargeObject(region->regionEnd - region->RegionStart());
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
                func(reinterpret_cast<BaseObject*>(region->RegionStart()));
            }
        });
    }
}

// ============================================================================
// Pre-STW MarkSetup: Stop all sweeps, clear markBitmaps, reclassify lists
// ============================================================================

void CentralRegionPool::StopAndWaitForAllSweep()
{
    // 1. Signal background sweep to stop at next check point
    stopSweepRequested_.store(true, std::memory_order_release);

    // 2. Disable mutator-initiated sweep paths in GetRegion / assist.
    skipLazySweep_.store(true, std::memory_order_release);
    skipAssistSweep_.store(true, std::memory_order_release);
    skipHelperSweep_.store(true, std::memory_order_release);

    // Pair with the seq_cst fences on the lazy-sweep / assist-sweep paths: this
    // makes the skip-store / active*SweepCount-load a sequentially consistent
    // StoreLoad, so any thread that already entered SweepRegion is guaranteed
    // visible in the waits below. Without it, ClearAllMarkBitmaps (called right
    // after) could clear a markBitmap while a mutator-side SweepRegion is still
    // reading it.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // 3. Wait for background sweep to finish its current iteration
    while (bgSweepRunning_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // 4. Wait for all in-flight lazy sweeps to complete.
    //    After skipLazySweep_=true, stopSweepRequested_=true and the counter
    //    is zero, no SweepRegion is running and no new one can start.
    while (activeLazySweepCount_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
    while (activeAssistSweepCount_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
    while (activeHelperSweepCount_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    size_t unsweptBefore = GetTotalUnsweptCount();
    MSSweepCounts sweepCounts = GetSweepCounts();
    ForceFinishAllUnswept();
    MSSweepAssistStats pSweepStats = SnapshotSweepAssistStats();
    size_t unsweptAfter = GetTotalUnsweptCount();
    size_t activeBytes = GetActiveMSRegionCount() * MS_REGION_SIZE;
    size_t emptySwept = GetEmptySweptRegionCount() * MS_REGION_SIZE;
    size_t largeCache = GetLargeHotCacheRegionCount() * MS_REGION_SIZE;
    size_t totalRetained = activeBytes + emptySwept + largeCache;
    VLOG(REPORT, "[MS-GC] StopAndWaitForAllSweep: forceSwept=%zu->%zu unswept regions, "
         "sources(bg=%zu lazy=%zu assist=%zu helper=%zu) retained=%zuB "
         "activeBytes=%zuB emptySwept=%zuB largeCache=%zuB "
         "pDebt(calls=%llu debtAddedScaled=%llu creditPaidRegions=%llu "
         "assistPaidRegions=%llu maxDebtScaled=%llu)",
         unsweptBefore, unsweptAfter,
         sweepCounts.backgroundSweptRegions, sweepCounts.lazySweptRegions,
         sweepCounts.assistSweptRegions, sweepCounts.helperSweptRegions,
         totalRetained, activeBytes, emptySwept, largeCache,
         static_cast<unsigned long long>(pSweepStats.calls),
         static_cast<unsigned long long>(pSweepStats.debtAddedScaled),
         static_cast<unsigned long long>(pSweepStats.creditPaidRegions),
         static_cast<unsigned long long>(pSweepStats.assistPaidRegions),
         static_cast<unsigned long long>(pSweepStats.maxDebtScaled));
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
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
