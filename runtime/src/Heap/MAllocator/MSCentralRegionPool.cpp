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
#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Collector/CollectorResources.h"
#include "Heap.h"
#include "Heap/MCollector/BackgroundProcessor.h"
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

#include <thread>

namespace MapleRuntime {

namespace {
size_t ReadEnvCount(const char* name, size_t fallback)
{
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return fallback;
    }
    size_t value = CString::ParsePosNumFromEnv(env);
    return value == 0 ? fallback : value;
}

double ReadEnvPositiveDecimal(const char* name, double fallback)
{
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return fallback;
    }
    double value = CString::ParsePosDecFromEnv(env);
    return value == 0.0 ? fallback : value;
}

uint64_t ReadEnvTimeUs(const char* name, uint64_t fallbackUs)
{
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return fallbackUs;
    }
    uint64_t valueNs = CString::ParseTimeFromEnv(env);
    if (valueNs == 0) {
        return fallbackUs;
    }
    return std::max<uint64_t>(1, valueNs / 1000ULL);
}

size_t GetBgSweepMaxRegionsPerSlice()
{
    static const size_t value = ReadEnvCount("CJ_MS_BG_SWEEP_MAX_REGIONS_PER_SLICE", 64);
    return value;
}

uint64_t GetBgSweepMaxTimeUs()
{
    static const uint64_t value = ReadEnvTimeUs("CJ_MS_BG_SWEEP_MAX_TIME", 2000);
    return value;
}

size_t GetBgReclaimNotifyEmptyRegions()
{
    static const size_t value = ReadEnvCount("CJ_MS_BG_RECLAIM_NOTIFY_EMPTY_REGIONS", 100);
    return value;
}

size_t GetLazyEmptyCacheRefillLimit()
{
    static const size_t value = ReadEnvCount("CJ_MS_LAZY_EMPTY_CACHE_REFILL_LIMIT", 1);
    return value;
}

size_t GetEmptyCacheLowPerBuffer()
{
    static const size_t value = ReadEnvCount("CJ_MS_EMPTY_CACHE_LOW_PER_BUFFER", 2);
    return value;
}

size_t GetEmptyCacheLowFloorRegions()
{
    static const size_t value = ReadEnvCount("CJ_MS_EMPTY_CACHE_LOW_FLOOR_REGIONS", 32);
    return value;
}

size_t GetEmptyCacheLowCapRegions()
{
    static const size_t value = ReadEnvCount("CJ_MS_EMPTY_CACHE_LOW_CAP_REGIONS", 256);
    return value;
}

size_t GetEmptyCacheHighFactor()
{
    static const size_t value = ReadEnvCount("CJ_MS_EMPTY_CACHE_HIGH_FACTOR", 2);
    return value;
}

uint64_t GetEmptyScavengeMaxTimeUs()
{
    static const uint64_t value = ReadEnvTimeUs("CJ_MS_EMPTY_SCAVENGE_MAX_TIME", 4000);
    return value;
}

size_t GetEmptyScavengeMaxRegionsPerSlice()
{
    static const size_t value = ReadEnvCount("CJ_MS_EMPTY_SCAVENGE_MAX_REGIONS_PER_SLICE", 256);
    return value;
}

size_t GetReclaimSweepMaxRegionsPerSlice()
{
    static const size_t value = ReadEnvCount("CJ_MS_RECLAIM_SWEEP_MAX_REGIONS_PER_SLICE", 512);
    return value;
}

uint64_t GetReclaimSweepMaxTimeUs()
{
    static const uint64_t value = ReadEnvTimeUs("CJ_MS_RECLAIM_SWEEP_MAX_TIME", 2000);
    return value;
}

double GetReclaimTrueLiveTargetRatio()
{
    static const double value = ReadEnvPositiveDecimal("CJ_MS_RECLAIM_TRUE_LIVE_TARGET_RATIO", 1.2);
    return value;
}

size_t GetEagerSweepBatchRegions()
{
    static const size_t value = ReadEnvCount("CJ_MS_EAGER_SWEEP_BATCH", 32);
    return value;
}
}

// Global sweep generation counter (starts at 2 so that sg-2 == 0 is valid initial state)
std::atomic<uint32_t> CentralRegionPool::globalSweepGen_{2};

CentralRegionPool::CentralRegionPool(MSRegionManager* msRegionManager)
    : msRegionManager_(msRegionManager) {}

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

    struct SweepClassHotSnapshot {
        uint32_t primaryCount = 0;
        uint32_t secondaryCount = 0;
        uint32_t score = 0;
    };
    std::array<SweepClassHotSnapshot, kNumSizeClasses> hot {};
    std::vector<AllocBufferMS*> buffers;
    {
        std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
        buffers = msAllocBuffers_;
    }
    for (AllocBufferMS* buffer : buffers) {
        if (buffer == nullptr) {
            continue;
        }
        for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
            uint16_t score = buffer->SnapshotHotClassScore(cls);
            hot[cls].score += score;
            AllocBufferMS::HotClassLevel level = buffer->SnapshotHotClassLevel(cls);
            if (level == AllocBufferMS::HotClassLevel::PRIMARY) {
                ++hot[cls].primaryCount;
            } else if (level == AllocBufferMS::HotClassLevel::SECONDARY) {
                ++hot[cls].secondaryCount;
            }
        }
    }

    std::sort(order.begin(), order.end(), [this, preferredCls, &hot](uint8_t lhs, uint8_t rhs) {
        bool lhsPreferred = preferredCls < kNumSizeClasses && lhs == preferredCls;
        bool rhsPreferred = preferredCls < kNumSizeClasses && rhs == preferredCls;
        if (lhsPreferred != rhsPreferred) {
            return lhsPreferred;
        }
        if (hot[lhs].primaryCount != hot[rhs].primaryCount) {
            return hot[lhs].primaryCount > hot[rhs].primaryCount;
        }
        if (hot[lhs].secondaryCount != hot[rhs].secondaryCount) {
            return hot[lhs].secondaryCount > hot[rhs].secondaryCount;
        }
        if (hot[lhs].score != hot[rhs].score) {
            return hot[lhs].score > hot[rhs].score;
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
// GetRegion — five-priority Region acquisition
// Priority: partialSwept -> unswept(lazy sweep) -> emptySweptCache(rebind)
//         -> fresh
// ============================================================================
MSRegionInfo* CentralRegionPool::GetRegion(uint8_t cls, MSRegionAcquireSource* source)
{
    SizeClassBucket& bucket = buckets_[cls];
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    size_t lazyEmptyCacheInsertions = 0;

    if (source != nullptr) {
        *source = MSRegionAcquireSource::NONE;
    }

    auto RebindEmptyRegionForClass = [&](MSRegionInfo* region) -> MSRegionInfo* {
        size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
        uintptr_t regionStart = MSRegionInfoArray::GetRegionAddress(regionIdx);

        if (region->sizeClass == cls && region->allocBitmap != nullptr) {
            region->ResetToBump();
        } else {
            region->ResetToEmptyCache();
            region->InitForSizeClass(cls);
            CHECK_DETAIL(region->allocBitmap != nullptr,
                         "GetRegion: InitForSizeClass failed for cls=%u idx=%zu",
                         static_cast<unsigned>(cls), regionIdx);
        }

        region->allocPtr = regionStart;
        region->regionEnd = regionStart + MS_REGION_SIZE;
        region->SetSweepGen(sg);
        return region;
    };

    // ---- Priority 1: partialSwept — already swept, fastest path ----
    {
        MSRegionInfo* region = bucket.partialSwept.Pop();
        if (region != nullptr) {
            if (source != nullptr) {
                *source = MSRegionAcquireSource::PARTIAL_SWEPT;
            }
            size_t ridx = MSRegionInfoArray::GetRegionIdx(region);
            uintptr_t expectedEnd = MSRegionInfoArray::GetRegionAddress(ridx) + MS_REGION_SIZE;
            if (UNLIKELY(region->regionEnd != expectedEnd)) {
                LOG(RTLOG_FATAL, "[MS-GC] GetRegion P1: partialSwept region has wrong regionEnd! "
                    "regionEnd=0x%zx, expected=0x%zx, allocPtr=0x%zx, cls=%u, "
                    "sizeClass=%u, allocBitmap=%p, this=%p (idx=%zu)",
                    region->regionEnd, expectedEnd, region->allocPtr,
                    (unsigned)cls, (unsigned)region->sizeClass,
                    (void*)region->allocBitmap, (void*)region, ridx);
            }
            return region;
        }
    }

    // ---- Priority 2: partialUnswept — sweep first, likely has free slots ----
    if (!skipLazySweep_.load(std::memory_order_acquire)) {
        // Notify background sweep only when we actually enter unswept paths,
        // rather than speculatively at GetRegion entry.
        MSAllocBuffer_RecordCentralSweepNotify();
        msRegionManager_->MaybeNotifySweep(this, cls);
        activeLazySweepCount_.fetch_add(1, std::memory_order_relaxed);
        // Double-check after increment to close the race with StopAndWaitForAllSweep
        if (skipLazySweep_.load(std::memory_order_acquire)) {
            activeLazySweepCount_.fetch_sub(1, std::memory_order_relaxed);
        } else {
            MSRegionInfo* sweptRegion = nullptr;
            for (;;) {
                MSRegionInfo* region;
                {
                    std::lock_guard<std::mutex> lg(bucket.lock);
                    region = bucket.partialUnswept.TakeHead();
                }
                if (region == nullptr) {
                    break;
                }

                // CAS to claim sweep ownership: sg-2 → sg-1 (sweeping)
                if (region->CasSweepGen(sg - 2, sg - 1)) {
                    size_t freedBytes = region->SweepRegion();
                    region->SetSweepGen(sg);
                    sweepStats_.lazySweptRegions.fetch_add(1, std::memory_order_relaxed);
                    sweepStats_.sweptByClass[cls].fetch_add(1, std::memory_order_relaxed);
                    sweepStats_.sweptFreedBytes.fetch_add(freedBytes, std::memory_order_relaxed);

                    if (region->IsCompletelyFree()) {
                        sweepStats_.emptiedRegions.fetch_add(1, std::memory_order_relaxed);
                        if (lazyEmptyCacheInsertions < GetLazyEmptyCacheRefillLimit()) {
                            // Mutator contributes a bounded amount of empty-cache refill.
                            PushEmptySweptRegion(region);
                            lazyEmptyCacheInsertions++;
                            continue;
                        }
                        sweptRegion = RebindEmptyRegionForClass(region);
                        break;
                    }
                    sweepStats_.retainedRegions.fetch_add(1, std::memory_order_relaxed);
                    region->regionType = MSRegionType::MS_REGION_PARTIAL;
                    sweptRegion = region;
                    break;
                }
                // CAS failed: region was stamped by MarkSetup or swept by another thread.
                // Return to partialSwept to avoid leaking the region.
                bucket.partialSwept.Push(region);
            }
            activeLazySweepCount_.fetch_sub(1, std::memory_order_relaxed);
            if (sweptRegion != nullptr) {
                if (source != nullptr) {
                    *source = MSRegionAcquireSource::PARTIAL_UNSWEPT;
                }
                return sweptRegion;
            }
        } // end double-check else
    } // end skipLazySweep check

    // ---- Priority 3: fullUnswept — sweep, may produce free slots ----
    if (!skipLazySweep_.load(std::memory_order_acquire)) {
        activeLazySweepCount_.fetch_add(1, std::memory_order_relaxed);
        if (skipLazySweep_.load(std::memory_order_acquire)) {
            activeLazySweepCount_.fetch_sub(1, std::memory_order_relaxed);
        } else {
            MSRegionInfo* sweptRegion = nullptr;
            for (;;) {
                MSRegionInfo* region;
                {
                    std::lock_guard<std::mutex> lg(bucket.lock);
                    region = bucket.fullUnswept.TakeHead();
                }
                if (region == nullptr) {
                    break;
                }

                if (region->CasSweepGen(sg - 2, sg - 1)) {
                    size_t freedBytes = region->SweepRegion();
                    region->SetSweepGen(sg);
                    sweepStats_.lazySweptRegions.fetch_add(1, std::memory_order_relaxed);
                    sweepStats_.sweptByClass[cls].fetch_add(1, std::memory_order_relaxed);
                    sweepStats_.sweptFreedBytes.fetch_add(freedBytes, std::memory_order_relaxed);

                    if (region->IsCompletelyFree()) {
                        sweepStats_.emptiedRegions.fetch_add(1, std::memory_order_relaxed);
                        if (lazyEmptyCacheInsertions < GetLazyEmptyCacheRefillLimit()) {
                            PushEmptySweptRegion(region);
                            lazyEmptyCacheInsertions++;
                            continue;
                        }
                        sweptRegion = RebindEmptyRegionForClass(region);
                        break;
                    }
                    sweepStats_.retainedRegions.fetch_add(1, std::memory_order_relaxed);
                    if (region->HasFreeSpace()) {
                        region->regionType = MSRegionType::MS_REGION_PARTIAL;
                        sweptRegion = region;
                        break;
                    }
                    // Still full after sweep → move to fullSwept
                    {
                        std::lock_guard<std::mutex> lg(bucket.lock);
                        bucket.fullSwept.Prepend(region);
                    }
                    continue;
                }
                // CAS failed: region was stamped. Return to fullSwept.
                {
                    std::lock_guard<std::mutex> lg(bucket.lock);
                    bucket.fullSwept.Prepend(region);
                }
            }
            activeLazySweepCount_.fetch_sub(1, std::memory_order_relaxed);
            if (sweptRegion != nullptr) {
                if (source != nullptr) {
                    *source = MSRegionAcquireSource::FULL_UNSWEPT;
                }
                return sweptRegion;
            }
        } // end double-check else
    } // end skipLazySweep check

    // ---- Priority 4: global empty cache — classless empty region shell ----
    {
        MSRegionInfo* region = TakeEmptySweptRegion(cls);
        if (region != nullptr) {
            region->SetSweepGen(sg);
            if (source != nullptr) {
                *source = MSRegionAcquireSource::EMPTY_REBIND;
            }
            return region;
        }
    }

    // ---- Priority 5: no cached Regions available, caller should GetFreshRegion ----
    return nullptr;
}

// ============================================================================
// GetFreshRegion — allocate a brand-new Region from MSRegionManager
// ============================================================================
MSRegionInfo* CentralRegionPool::GetFreshRegion(uint8_t cls)
{
    // MSRegionManager::TakeRegion allocates one MS_REGION_SIZE region
    // and returns the corresponding MSRegionInfo* directly (one-level).
    MSRegionInfo* msInfo = msRegionManager_->TakeRegion(cls, *this);
    if (msInfo == nullptr) {
        return nullptr;
    }

    // Compute region start address from MSRegionInfo index
    size_t regionIdx = MSRegionInfoArray::GetRegionIdx(msInfo);
    uintptr_t regionStart = MSRegionInfoArray::GetRegionAddress(regionIdx);

    // Debug: check MSRegionInfo state BEFORE InitForSizeClass
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

    // Initialize for the requested size class (sets bitmaps, slot counts, state)
    msInfo->InitForSizeClass(cls);

    // Debug: verify bitmap allocation succeeded
    if (UNLIKELY(msInfo->allocBitmap == nullptr)) {
        LOG(RTLOG_FATAL, "[MS-GC] GetFreshRegion: InitForSizeClass failed to alloc bitmap, "
            "cls=%u, regionIdx=%zu, regionStart=0x%zx, totalSlots=%u, this=%p",
            (unsigned)cls, regionIdx, regionStart, (unsigned)msInfo->totalSlotCount,
            (void*)msInfo);
    }

    // Set addresses that InitForSizeClass does not set
    msInfo->allocPtr = regionStart;
    msInfo->regionEnd = regionStart + MS_REGION_SIZE;

    // Stamp the current sweep generation so this Region is seen as "already swept"
    msInfo->SetSweepGen(globalSweepGen_.load(std::memory_order_acquire));

    // Track total MS region bytes for GC trigger
    totalMSRegionBytes_.fetch_add(MS_REGION_SIZE, std::memory_order_relaxed);

    return msInfo;
}

// ============================================================================
// Return / Release — Region return from thread-local cache
// ============================================================================

void CentralRegionPool::ReturnPartialRegion(MSRegionInfo* region, uint8_t cls)
{
    SizeClassBucket& bucket = buckets_[cls];
    // O3: preserve region type (BUMP or PARTIAL) — don't force PARTIAL
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    if (region->GetSweepGen() == sg) {
        bucket.partialSwept.Push(region);  // O1: lock-free
    } else {
        std::lock_guard<std::mutex> lg(bucket.lock);
        bucket.partialUnswept.Prepend(region);
    }
}

void CentralRegionPool::ReturnFullRegion(MSRegionInfo* region, uint8_t cls)
{
    SizeClassBucket& bucket = buckets_[cls];
    std::lock_guard<std::mutex> lg(bucket.lock);
    region->regionType = MSRegionType::MS_REGION_FULL;
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    if (region->GetSweepGen() == sg) {
        bucket.fullSwept.Prepend(region);
    } else {
        bucket.fullUnswept.Prepend(region);
    }
}

void CentralRegionPool::ReturnEmptyRegion(MSRegionInfo* region)
{
    PushEmptySweptRegion(region);
}

void CentralRegionPool::ReleaseRegion(MSRegionInfo* region, uint8_t /* cls */)
{
    ReleaseRegionInternal(region);
}

void CentralRegionPool::ReleaseRegionInternal(MSRegionInfo* region)
{
    region->Reset();

    // Track total MS region bytes for GC trigger
    totalMSRegionBytes_.fetch_sub(MS_REGION_SIZE, std::memory_order_relaxed);

    // Return the underlying physical Region to MSRegionManager's free pool.
    // One-level: MSRegionManager directly manages the region via MSRegionInfo*.
    msRegionManager_->ReclaimRegion(region);
}

void CentralRegionPool::PushEmptySweptRegion(MSRegionInfo* region)
{
    CHECK_DETAIL(region != nullptr, "PushEmptySweptRegion with null region");
    region->ResetToEmptyCache();
    {
        std::lock_guard<std::mutex> lg(emptySweptLock_);
        emptySweptCache_.Prepend(region);
        emptySweptRegionCount_.fetch_add(1, std::memory_order_relaxed);
        emptySweptBytes_.fetch_add(MS_REGION_SIZE, std::memory_order_relaxed);
    }
    if (msRegionManager_ != nullptr && ShouldNotifyBackgroundReclaim()) {
        msRegionManager_->MaybeNotifyReclaim();
    }
}

MSRegionInfo* CentralRegionPool::TakeEmptySweptRegion(uint8_t cls)
{
    MSRegionInfo* region = nullptr;
    {
        std::lock_guard<std::mutex> lg(emptySweptLock_);
        region = emptySweptCache_.TakeHead();
        if (region == nullptr) {
            return nullptr;
        }
        emptySweptRegionCount_.fetch_sub(1, std::memory_order_relaxed);
        emptySweptBytes_.fetch_sub(MS_REGION_SIZE, std::memory_order_relaxed);
    }

    CHECK_DETAIL(region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE,
                 "TakeEmptySweptRegion got bad regionType=%u",
                 static_cast<unsigned>(region->regionType));

    size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
    uintptr_t regionStart = MSRegionInfoArray::GetRegionAddress(regionIdx);
    region->InitForSizeClass(cls);
    CHECK_DETAIL(region->allocBitmap != nullptr,
                 "TakeEmptySweptRegion: InitForSizeClass failed for cls=%u idx=%zu",
                 static_cast<unsigned>(cls), regionIdx);
    region->allocPtr = regionStart;
    region->regionEnd = regionStart + MS_REGION_SIZE;
    return region;
}

MSRegionInfo* CentralRegionPool::TakeEmptySweptRegionForReclaim()
{
    std::lock_guard<std::mutex> lg(emptySweptLock_);
    MSRegionInfo* region = emptySweptCache_.TakeHead();
    if (region == nullptr) {
        return nullptr;
    }
    emptySweptRegionCount_.fetch_sub(1, std::memory_order_relaxed);
    emptySweptBytes_.fetch_sub(MS_REGION_SIZE, std::memory_order_relaxed);
    return region;
}

void CentralRegionPool::DecrementTotalMSRegionBytes(size_t bytes)
{
    totalMSRegionBytes_.fetch_sub(bytes, std::memory_order_relaxed);
}

size_t CentralRegionPool::GetEmptyCacheLowWatermarkRegions() const
{
    size_t low = std::max(GetAllocBufferCount() * GetEmptyCacheLowPerBuffer(), GetEmptyCacheLowFloorRegions());
    return std::min(low, GetEmptyCacheLowCapRegions());
}

size_t CentralRegionPool::GetEmptyCacheHighWatermarkRegions() const
{
    return GetEmptyCacheLowWatermarkRegions() * GetEmptyCacheHighFactor();
}

bool CentralRegionPool::ShouldNotifyBackgroundReclaim() const
{
    return GetEmptySweptRegionCount() >= GetBgReclaimNotifyEmptyRegions();
}

size_t CentralRegionPool::GetTrueLiveSlackTargetBytes() const
{
    const GCStats& gcStats = Heap::GetHeap().GetCollector().GetGCStats();
    size_t gcThreshold = gcStats.GetThreshold();
    if (gcStats.lastTrueLiveBytes == 0 || gcThreshold == 0) {
        return 0;
    }
    size_t targetBytes = static_cast<size_t>(static_cast<double>(gcStats.lastTrueLiveBytes) *
                                             GetReclaimTrueLiveTargetRatio());
    targetBytes = std::max(targetBytes, gcStats.lastTrueLiveBytes);
    return std::min(targetBytes, gcThreshold);
}

bool CentralRegionPool::ShouldScavengeForTrueLiveSlack(size_t committedBytes, size_t targetBytes) const
{
    return targetBytes != 0 && committedBytes > targetBytes;
}

bool CentralRegionPool::IsReclaimPressureHigh() const
{
    return ShouldScavengeForTrueLiveSlack(GetTotalMSRegionBytes(), GetTrueLiveSlackTargetBytes());
}

size_t CentralRegionPool::GetActiveWorkingSetRegionCount() const
{
    // Count regions that are immediately part of the current working set:
    // mutator-active regions + alloc-buffer cached spares + central ready regions.
    // Exclude empty cache and released spans because they do not actively grow RSS pressure.
    return GetAllAllocBufferActiveRegionCount() +
           GetAllAllocBufferCachedRegionCount() +
           GetTotalPartialSweptCount() +
           GetTotalFullCount() +
           GetLargeRegionSpanCount();
}

MSFeedPressureLevel CentralRegionPool::GetFeedPressureLevel() const
{
    // Thresholds are computed dynamically from the current committed heap capacity,
    // so they automatically scale when the heap grows or shrinks.
    // Ratios (using integer cross-multiplication to avoid division):
    //   MEDIUM  : activeWorkingSet >= 1/2 of totalRegions
    //   HIGH    : activeWorkingSet >= 2/3 of totalRegions
    //   EXTREME : activeWorkingSet >= 4/5 of totalRegions
    size_t totalRegions = (msRegionManager_ != nullptr) ? msRegionManager_->GetMaxRegionCount() : 0;
    size_t activeWorkingSet = GetActiveWorkingSetRegionCount();

    if (totalRegions == 0) {
        return MSFeedPressureLevel::LOW;
    }
    if (activeWorkingSet * 5 >= totalRegions * 4) {
        return MSFeedPressureLevel::EXTREME;
    }
    if (activeWorkingSet * 3 >= totalRegions * 2) {
        return MSFeedPressureLevel::HIGH;
    }
    if (activeWorkingSet * 2 >= totalRegions) {
        return MSFeedPressureLevel::MEDIUM;
    }
    return MSFeedPressureLevel::LOW;
}

size_t CentralRegionPool::HandleBackgroundScavenge(bool releaseAll)
{
    size_t emptyCount = GetEmptySweptRegionCount();
    if (emptyCount == 0) {
        return 0;
    }

    if (!releaseAll && !ShouldNotifyBackgroundReclaim()) {
        return 0;
    }

    uint64_t deadlineUs = TimeUtil::MicroSeconds() + GetBgSweepMaxTimeUs();
    size_t targetRegions = releaseAll ? emptyCount : std::max<size_t>(1, emptyCount / 2);
    size_t maxRegions = releaseAll ? emptyCount : std::min(targetRegions, GetBgSweepMaxRegionsPerSlice());
    size_t releasedRegions = 0;
    size_t releasedBytes = 0;

    while (releasedRegions < maxRegions &&
           (releaseAll || TimeUtil::MicroSeconds() < deadlineUs)) {
        MSRegionInfo* region = nullptr;
        {
            std::lock_guard<std::mutex> lg(emptySweptLock_);
            region = emptySweptCache_.TakeHead();
            if (region == nullptr) {
                break;
            }
            emptySweptRegionCount_.fetch_sub(1, std::memory_order_relaxed);
            emptySweptBytes_.fetch_sub(MS_REGION_SIZE, std::memory_order_relaxed);
        }

        msRegionManager_->ReclaimEmptyRegionToReleased(region);
        totalMSRegionBytes_.fetch_sub(MS_REGION_SIZE, std::memory_order_relaxed);
        releasedRegions++;
        releasedBytes += MS_REGION_SIZE;
    }

    if (releasedRegions > 0) {
        VLOG(REPORT,
             "[MS-GC] BackgroundScavenge slice: released=%zu regions (%s B), emptyCache=%zu->%zu regions, target=%zu",
             releasedRegions, Pretty(releasedBytes).Str(),
             emptyCount, GetEmptySweptRegionCount(), targetRegions);
    }
    return releasedBytes;
}

bool CentralRegionPool::IsEmptyCacheLow() const
{
    return GetEmptySweptRegionCount() < GetEmptyCacheLowWatermarkRegions();
}

// ============================================================================
// GC operations
// ============================================================================

void CentralRegionPool::PrepareForSweep()
{
    sweepStats_.Reset();

    // At this point MarkSetup has already stopped sweep and promoted any residual
    // unswept regions to swept. FlushAllAllocBuffers returns TL regions to swept.
    // O(1) per-bucket flip:
    //   partialSwept → partialUnswept (StealChainTo: head pointer transfer)
    //   fullSwept    ↔ fullUnswept    (Swap: head/count exchange)
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket& bucket = buckets_[cls];
        std::lock_guard<std::mutex> lg(bucket.lock);
        bucket.partialSwept.StealChainTo(bucket.partialUnswept);
        bucket.fullSwept.Swap(bucket.fullUnswept);
    }

    globalSweepGen_.fetch_add(2, std::memory_order_release);
    sweepDone_.store(false, std::memory_order_release);
}

// ============================================================================
// BackgroundSweep — called by gc-helper thread (FinalizerProcessor)
// ============================================================================

void CentralRegionPool::BackgroundSweep(uint8_t preferredCls)
{
    bgSweepRunning_.store(true, std::memory_order_release);
    auto finish = [this]() {
        bgSweepRunning_.store(false, std::memory_order_release);
    };

    if (stopSweepRequested_.load(std::memory_order_acquire)) {
        finish();
        return;
    }

    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    uint64_t t0 = TimeUtil::MicroSeconds();
    size_t maxRegions = GetBgSweepMaxRegionsPerSlice();
    size_t remainingUnswept = GetTotalUnsweptCount();
    if (remainingUnswept == 0) {
        sweepDone_.store(true, std::memory_order_release);
        finish();
        return;
    }
    uint64_t deadlineUs = t0 + GetBgSweepMaxTimeUs();
    size_t sweptInSlice = 0;
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
        if (!SweepUnsweptList(bucket, bucket.partialUnswept, sg, sweptInSlice, maxRegions, deadlineUs)) {
            pendingWork = !stopSweepRequested_.load(std::memory_order_acquire);
            return false;
        }
        if (!SweepUnsweptList(bucket, bucket.fullUnswept, sg, sweptInSlice, maxRegions, deadlineUs)) {
            pendingWork = !stopSweepRequested_.load(std::memory_order_acquire);
            return false;
        }
        if (cls % 4 == 3) {
            std::this_thread::yield();
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
    sweepStats_.bgSweepTimeUs.fetch_add(elapsed, std::memory_order_relaxed);
    if (sweptInSlice > 0 || pendingWork) {
        VLOG(REPORT, "[MS-GC] BackgroundSweep slice: swept=%zu regions, emptied=%zu, elapsed=%sus, pending=%s",
             sweptInSlice,
             sweepStats_.emptiedRegions.load(std::memory_order_relaxed),
             Pretty(elapsed).Str(),
             pendingWork ? "yes" : "no");
    }

    if (!pendingWork && !stopSweepRequested_.load(std::memory_order_acquire)) {
        sweepDone_.store(true, std::memory_order_release);
    }
    finish();
}

bool CentralRegionPool::SweepUnsweptList(SizeClassBucket& bucket, MSRegionList& list, uint32_t sg,
                                         size_t& sweptInSlice, size_t maxRegions, uint64_t deadlineUs)
{
    for (;;) {
        // Check stop request per-region for prompt exit
        if (stopSweepRequested_.load(std::memory_order_acquire)) {
            return false;
        }

        if (sweptInSlice >= maxRegions || TimeUtil::MicroSeconds() >= deadlineUs) {
            return false;
        }

        MSRegionInfo* region;
        {
            std::lock_guard<std::mutex> lg(bucket.lock);
            region = list.TakeHead();
        }
        if (region == nullptr) {
            break;
        }

        // CAS to claim sweep ownership (coordinate with Lazy Sweep / BackgroundSweep)
        if (region->CasSweepGen(sg - 2, sg - 1)) {
            size_t freedBytes = region->SweepRegion();
            region->SetSweepGen(sg);
            sweptInSlice++;
            sweepStats_.bgSweptRegions.fetch_add(1, std::memory_order_relaxed);
            sweepStats_.sweptByClass[region->sizeClass].fetch_add(1, std::memory_order_relaxed);
            sweepStats_.sweptFreedBytes.fetch_add(freedBytes, std::memory_order_relaxed);

            if (region->IsCompletelyFree()) {
                // Completely empty regions leave the class-local supply and
                // become classless empty-cache entries.
                PushEmptySweptRegion(region);
                sweepStats_.emptiedRegions.fetch_add(1, std::memory_order_relaxed);
            } else if (region->HasFreeSpace()) {
                sweepStats_.retainedRegions.fetch_add(1, std::memory_order_relaxed);
                region->regionType = MSRegionType::MS_REGION_PARTIAL;
                bucket.partialSwept.Push(region);  // O1: lock-free
            } else {
                sweepStats_.retainedRegions.fetch_add(1, std::memory_order_relaxed);
                region->regionType = MSRegionType::MS_REGION_FULL;
                std::lock_guard<std::mutex> lg(bucket.lock);
                bucket.fullSwept.Prepend(region);
            }

            if ((sweptInSlice & 0x7U) == 0) {
                std::this_thread::yield();
            }
        }
        // CAS failed: Lazy Sweep or ForceFinish grabbed it, skip
    }

    return true;
}

// ===== Monitoring =====

size_t CentralRegionPool::GetTotalPartialCount() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        total += buckets_[cls].partialSwept.Count();
        total += buckets_[cls].partialUnswept.Count();
    }
    return total;
}

size_t CentralRegionPool::GetTotalFullCount() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        total += buckets_[cls].fullSwept.Count();
        total += buckets_[cls].fullUnswept.Count();
    }
    return total;
}

size_t CentralRegionPool::GetTotalUnsweptCount() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        total += buckets_[cls].partialUnswept.Count();
        total += buckets_[cls].fullUnswept.Count();
    }
    return total;
}

size_t CentralRegionPool::GetTotalSweptCount() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        total += buckets_[cls].partialSwept.Count();
        total += buckets_[cls].fullSwept.Count();
    }
    return total;
}

size_t CentralRegionPool::GetTotalPartialSweptCount() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        total += buckets_[cls].partialSwept.Count();
    }
    return total;
}

size_t CentralRegionPool::GetTotalFullUnsweptCount() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        total += buckets_[cls].fullUnswept.Count();
    }
    return total;
}

size_t CentralRegionPool::GetLargeRegionCount() const
{
    return largeRegionList_.Count();
}

size_t CentralRegionPool::GetLargeRegionSpanCount() const
{
    size_t totalRegions = 0;
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(largeLock_));
    largeRegionList_.ForEach([&totalRegions](MSRegionInfo* region) {
        totalRegions += (region->regionEnd - region->RegionStart()) / MS_REGION_SIZE;
    });
    return totalRegions;
}

    void CentralRegionPool::ForceFinishUnsweptList(SizeClassBucket& bucket, MSRegionList& list, uint8_t cls, uint32_t sg,
                                                   size_t& sweptRegions, size_t& emptiedRegions,
                                                   size_t& retainedRegions, size_t& freedBytes)
    {
        for (;;) {
            MSRegionInfo* region;
            {
                std::lock_guard<std::mutex> lg(bucket.lock);
                region = list.TakeHead();
            }
            if (region == nullptr) {
                break;
            }

            if (UNLIKELY(!region->CasSweepGen(sg - 2, sg - 1))) {
                LOG(RTLOG_FATAL,
                    "[MS-GC] ForceFinishUnsweptList: unexpected sweepGen transition failure, "
                    "cls=%u region=%p sweepGen=%u type=%u",
                    static_cast<unsigned>(cls), static_cast<void*>(region),
                    static_cast<unsigned>(region->GetSweepGen()),
                    static_cast<unsigned>(region->regionType));
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
                bucket.partialSwept.Push(region);
                continue;
            }

            region->regionType = MSRegionType::MS_REGION_FULL;
            std::lock_guard<std::mutex> lg(bucket.lock);
            bucket.fullSwept.Prepend(region);
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
            ForceFinishUnsweptList(bucket, bucket.partialUnswept, cls, sg,
                                   sweptRegions, emptiedRegions, retainedRegions, freedBytes);
            ForceFinishUnsweptList(bucket, bucket.fullUnswept, cls, sg,
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

size_t CentralRegionPool::GetAllAllocBufferRegionCount() const
{
    return GetAllAllocBufferActiveRegionCount() + GetAllAllocBufferCachedRegionCount();
}

CentralRegionPool::BucketPressure CentralRegionPool::GetBucketPressure(uint8_t cls) const
{
    BucketPressure pressure;
    const SizeClassBucket& bucket = buckets_[cls];

    pressure.partialSwept = bucket.partialSwept.Count();
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(bucket.lock));
    pressure.fullSwept = bucket.fullSwept.Count();
    pressure.partialUnswept = bucket.partialUnswept.Count();
    pressure.fullUnswept = bucket.fullUnswept.Count();
    return pressure;
}

void CentralRegionPool::DumpPoolStats() const
{
    size_t partialSweptTotal = 0;
    size_t fullSweptTotal = 0;
    size_t partialUnsweptTotal = 0;
    size_t fullUnsweptTotal = 0;
    size_t emptySweptTotal = GetEmptySweptRegionCount();

    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        partialSweptTotal += buckets_[cls].partialSwept.Count();
        fullSweptTotal += buckets_[cls].fullSwept.Count();
        partialUnsweptTotal += buckets_[cls].partialUnswept.Count();
        fullUnsweptTotal += buckets_[cls].fullUnswept.Count();
    }

    size_t totalSmall = partialSweptTotal + fullSweptTotal + partialUnsweptTotal + fullUnsweptTotal +
                        emptySweptTotal;

    VLOG(REPORT, "[MS-GC]   CentralPool small (%u classes aggregated):", kNumSizeClasses);
    VLOG(REPORT, "[MS-GC]     partialSwept: %zu regions  fullSwept: %zu regions",
         partialSweptTotal, fullSweptTotal);
    VLOG(REPORT, "[MS-GC]     partialUnswept: %zu regions  fullUnswept: %zu regions",
         partialUnsweptTotal, fullUnsweptTotal);
    VLOG(REPORT, "[MS-GC]     emptySweptCache: %zu regions", emptySweptTotal);
    VLOG(REPORT, "[MS-GC]     total small: %zu regions", totalSmall);
    VLOG(REPORT, "[MS-GC]     large regions: %zu", largeRegionList_.Count());
}

// ============================================================================
// Large Region management (Tier 1 in-heap large objects)
// ============================================================================

void CentralRegionPool::RegisterLargeRegion(MSRegionInfo* region)
{
    std::lock_guard<std::mutex> lg(largeLock_);
    largeRegionList_.Prepend(region);
}

// EagerSweepBatch — synchronous GC-thread sweep of a batch of unswept regions.
// Called from ConcurrentSweep() before handing off to the background thread,
// giving immediate reclaim under high allocation pressure.
// Uses the same CAS ownership protocol as BackgroundSweep / LazySweep.
size_t CentralRegionPool::EagerSweepBatch(size_t maxRegions)
{
    if (maxRegions == 0) {
        return 0;
    }
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    uint64_t deadlineUs = TimeUtil::MicroSeconds() + 5000;  // 5ms hard cap
    size_t swept = 0;

    for (uint8_t cls = 0; cls < kNumSizeClasses && swept < maxRegions; cls++) {
        SizeClassBucket& bucket = buckets_[cls];
        // Sweep partialUnswept first (most likely to yield free slots)
        if (!SweepUnsweptList(bucket, bucket.partialUnswept, sg, swept, maxRegions, deadlineUs)) {
            break;
        }
        if (!SweepUnsweptList(bucket, bucket.fullUnswept, sg, swept, maxRegions, deadlineUs)) {
            break;
        }
    }

    if (swept > 0) {
        VLOG(REPORT, "[MS-GC] EagerSweepBatch: swept %zu regions (limit=%zu)", swept, maxRegions);
    }
    return swept;
}

MSLargeSweepStats CentralRegionPool::SweepLargeRegions()
{
    // Two-pass to avoid holding largeLock_ during ReclaimRegion.
    std::vector<MSRegionInfo*> deadRegions;
    MSLargeSweepStats stats;

    {
        std::lock_guard<std::mutex> lg(largeLock_);
        // Iterate and remove dead regions from the list
        // We need to iterate using ForEach and collect dead ones
        std::vector<MSRegionInfo*> toRemove;
        largeRegionList_.ForEach([&toRemove](MSRegionInfo* region) {
            if (!region->IsMarked()) {
                toRemove.push_back(region);
            }
        });
        for (MSRegionInfo* region : toRemove) {
            largeRegionList_.Remove(region);
            deadRegions.push_back(region);
        }
    }

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

        // Reset base MSRegionInfo
        msInfo->Reset();

        // Return the physical region to MSRegionManager (one-level, no RegionInfo indirection)
        msRegionManager_->ReclaimRegionLarge(regionStart, regionBytes);

        // Adjust byte counter
        totalMSRegionBytes_.fetch_sub(regionBytes, std::memory_order_relaxed);
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
        msAllocBuffers_.erase(it);
    }
}

void CentralRegionPool::FlushAllAllocBuffers()
{
    std::vector<AllocBufferMS*> buffers;
    {
        std::lock_guard<std::mutex> lg(msAllocBufferLock_);
        buffers = msAllocBuffers_;
    }
    for (AllocBufferMS* buffer : buffers) {
        if (buffer != nullptr) {
            buffer->FlushAll();
        }
    }
}

BackgroundFeedStats CentralRegionPool::BackgroundFillAllocCaches(uint8_t preferredCls)
{
    BackgroundFeedStats totalStats;
    bool reclaimPressure = IsReclaimPressureHigh();
    MSFeedPressureLevel pressureLevel = GetFeedPressureLevel();
    totalStats.pressureLevel = static_cast<uint8_t>(pressureLevel);
    totalStats.activeWorkingSetRegions = GetActiveWorkingSetRegionCount();

    if (pressureLevel == MSFeedPressureLevel::EXTREME) {
        return totalStats;
    }

    std::vector<AllocBufferMS*> buffers;
    {
        std::lock_guard<std::mutex> lg(msAllocBufferLock_);
        buffers = msAllocBuffers_;
    }
    totalStats.buffersScanned = buffers.size();
    for (AllocBufferMS* buffer : buffers) {
        if (buffer != nullptr) {
            MSAllocCacheFillStats stats = buffer->BackgroundFillCache(preferredCls, reclaimPressure, pressureLevel);
            if (stats.HasWork()) {
                totalStats.touchedBuffers++;
                totalStats.filledClasses += stats.filledClasses;
                totalStats.filledRegions += stats.filledRegions;
            }
        }
    }
    return totalStats;
}

// ============================================================================
// ComputeMarkedBytes: walk markBitmaps to count true live bytes
// ============================================================================
// Called after marking completes, before PrepareForSweep (gen flip).
// At this point markBitmaps are stable: marking is done, no sweep has started.
//
size_t CentralRegionPool::ComputeMarkedBytes() const
{
    size_t totalLive = 0;

    // 1. Walk all small regions in CentralPool (all 4 lists × 36 classes)
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        const SizeClassBucket& bucket = buckets_[cls];
        size_t slotSize = SizeClassInfo::ClassToSize(cls);

        auto countRegion = [slotSize, &totalLive](MSRegionInfo* region) {
            if (region->markBitmap == nullptr || region->totalSlotCount == 0) {
                return;
            }
            size_t wordCount = (region->totalSlotCount + 63) / 64;
            int marked = 0;
            for (size_t i = 0; i < wordCount; i++) {
                marked += __builtin_popcountll(region->markBitmap[i]);
            }
            totalLive += static_cast<size_t>(marked) * slotSize;
        };

        bucket.partialSwept.ForEachUnsafe(countRegion);
        // fullSwept, partialUnswept, fullUnswept require bucket lock,
        // but we are called from GC thread after marking with no concurrent sweep.
        // Use const_cast to take lock (ForEach is const-safe for reading).
        const_cast<SizeClassBucket&>(bucket).lock.lock();
        bucket.fullSwept.ForEach(countRegion);
        bucket.partialUnswept.ForEach(countRegion);
        bucket.fullUnswept.ForEach(countRegion);
        const_cast<SizeClassBucket&>(bucket).lock.unlock();
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
                int marked = 0;
                for (size_t i = 0; i < wordCount; i++) {
                    marked += __builtin_popcountll(region->markBitmap[i]);
                }
                totalLive += static_cast<size_t>(marked) * slotSize;
            }
        }
    }

    // 3. Live large regions (isMarked == 1 after CollectLargeGarbage)
    {
        std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(largeLock_));
        largeRegionList_.ForEach([&totalLive](MSRegionInfo* region) {
            if (region->IsMarked()) {
                totalLive += region->regionEnd - region->RegionStart();
            }
        });
    }

    return totalLive;
}

// ============================================================================
// Pre-STW MarkSetup: Stop all sweeps, clear markBitmaps, reclassify lists
// ============================================================================

void CentralRegionPool::StopAndWaitForAllSweep()
{
    // 1. Signal background sweep to stop at next check point
    stopSweepRequested_.store(true, std::memory_order_release);

    // 2. Disable lazy sweep in GetRegion P2/P3
    skipLazySweep_.store(true, std::memory_order_release);

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

    if (GetTotalUnsweptCount() > 0) {
        ForceFinishAllUnswept();
    }
}

void CentralRegionPool::ClearAllMarkBitmaps()
{
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    size_t maxRegions = msRegionManager_->GetMaxRegionCount();

    size_t stampedCount = 0;
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
            info->ClearMarkedFlag();
            largeCleared++;
            continue;
        }

        // Skip regions with no bitmap (not yet initialized)
        if (info->regionEnd == 0) {
            continue;
        }

        // Stamp unswept regions as swept (CAS sg-2 -> sg) to prevent future
        // lazy sweep from running on stale markBitmaps.
        if (info->CasSweepGen(sg - 2, sg)) {
            stampedCount++;
        }

        // Clear markBitmap unconditionally for all active small/medium regions.
        // Safe because: no SweepRegion is running (StopAndWaitForAllSweep),
        // and mark-at-alloc is inactive (GC phase is IDLE/SWEEP).
        info->ClearMarkBitmap();
        clearedCount++;
    }

    VLOG(REPORT,
         "[MS-GC] ClearAllMarkBitmaps: clearedSmall=%zu regions, clearedLarge=%zu regions, "
         "unsweptPromoted=%zu regions, committedRegions=%zu",
         clearedCount, largeCleared, stampedCount, maxRegions);
}

void CentralRegionPool::ReclassifyUnsweptToSwept()
{
    size_t partialMoved = 0;
    size_t fullMoved = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket& bucket = buckets_[cls];
        std::lock_guard<std::mutex> lg(bucket.lock);

        // Drain partialUnswept -> partialSwept
        MSRegionInfo* r;
        while ((r = bucket.partialUnswept.TakeHead()) != nullptr) {
            bucket.partialSwept.Push(r);
            partialMoved++;
        }

        // Drain fullUnswept -> fullSwept
        while ((r = bucket.fullUnswept.TakeHead()) != nullptr) {
            bucket.fullSwept.Prepend(r);
            fullMoved++;
        }
    }

    VLOG(REPORT,
         "[MS-GC] ReclassifyUnsweptToSwept: partial=%zu regions, full=%zu regions, total=%zu",
         partialMoved, fullMoved, partialMoved + fullMoved);
}

void CentralRegionPool::ReenableLazySweep()
{
    skipLazySweep_.store(false, std::memory_order_release);
    stopSweepRequested_.store(false, std::memory_order_release);
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
