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
size_t ReadEnvCount(const char* name, size_t fallback)
{
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return fallback;
    }
    size_t value = CString::ParsePosNumFromEnv(env);
    return value == 0 ? fallback : value;
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

// 后台批量处理 region 数
size_t GetBgSweepMaxRegionsPerSlice(size_t unsweptBacklog)
{
    static const size_t baseValue = ReadEnvCount("CJ_MS_BG_SWEEP_MAX_REGIONS_PER_SLICE", 512);
    static const size_t highValue = ReadEnvCount("CJ_MS_BG_SWEEP_HIGH_MAX_REGIONS_PER_SLICE", 2048);
    static const size_t highBacklogThreshold = ReadEnvCount("CJ_MS_BG_SWEEP_HIGH_UNSWEPT", 10240);
    return unsweptBacklog > highBacklogThreshold ? std::max(baseValue, highValue) : baseValue;
}

// 后台批量处理超时时间
uint64_t GetBgSweepMaxTimeUs(size_t unsweptBacklog)
{
    static const uint64_t baseValue = ReadEnvTimeUs("CJ_MS_BG_SWEEP_MAX_TIME", 2000);
    static const uint64_t highValue = ReadEnvTimeUs("CJ_MS_BG_SWEEP_HIGH_MAX_TIME", 20000);
    static const size_t highBacklogThreshold = ReadEnvCount("CJ_MS_BG_SWEEP_HIGH_UNSWEPT", 10240);
    return unsweptBacklog > highBacklogThreshold ? std::max(baseValue, highValue) : baseValue;
}

void ClearSmallRegionPayload(uintptr_t regionStart)
{
    memset(reinterpret_cast<void*>(regionStart), 0, MS_REGION_SIZE);
}

}

// Global sweep generation counter (starts at 2 so that sg-2 == 0 is valid initial state)
std::atomic<uint32_t> CentralRegionPool::globalSweepGen_{2};

CentralRegionPool::CentralRegionPool(MSRegionManager* msRegionManager)
    : msRegionManager_(msRegionManager)
{}

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

    if (source != nullptr) {
        *source = MSRegionAcquireSource::NONE;
    }

    auto RebindEmptyRegionForClass = [&](MSRegionInfo* region) -> MSRegionInfo* {
        size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
        uintptr_t regionStart = MSRegionInfoArray::GetRegionAddress(regionIdx);
        ClearSmallRegionPayload(regionStart);

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
        MSRegionInfo* region = bucket.TakePartialSwept();
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
            activeLazySweepCount_.fetch_sub(1, std::memory_order_relaxed);
        } else {
            MSRegionInfo* sweptRegion = nullptr;
            for (;;) {
                MSRegionInfo* region = bucket.TakeUnswept(SizeClassBucket::UnsweptListKind::PARTIAL);
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
                bucket.PushPartialSwept(region);
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
        // seq_cst fence: same StoreLoad ordering requirement as Priority 2 above —
        // pairs with StopAndWaitForAllSweep so a claimed lazy sweep can never run
        // concurrently with ClearAllMarkBitmaps.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (skipLazySweep_.load(std::memory_order_acquire)) {
            activeLazySweepCount_.fetch_sub(1, std::memory_order_relaxed);
        } else {
            MSRegionInfo* sweptRegion = nullptr;
            for (;;) {
                MSRegionInfo* region = bucket.TakeUnswept(SizeClassBucket::UnsweptListKind::FULL);
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
                    bucket.PushFullSwept(region);
                    continue;
                }
                // CAS failed: region was stamped. Return to fullSwept.
                bucket.PushFullSwept(region);
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

    // ---- Priority 4: no cached Regions available, caller should GetFreshRegion ----
    return nullptr;
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

    // Set addresses that InitForSizeClass does not set
    msInfo->allocPtr = regionStart;
    msInfo->regionEnd = regionStart + MS_REGION_SIZE;

    // Stamp the current sweep generation so this Region is seen as "already swept"
    msInfo->SetSweepGen(globalSweepGen_.load(std::memory_order_acquire));

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
    SizeClassBucket& bucket = buckets_[cls];
    // O3: preserve region type (BUMP or PARTIAL) — don't force PARTIAL
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    if (region->GetSweepGen() == sg) {
        bucket.PushPartialSwept(region);
    } else {
        bucket.PushPartialUnswept(region);
    }
}

void CentralRegionPool::ReturnFullRegion(MSRegionInfo* region, uint8_t cls)
{
    SizeClassBucket& bucket = buckets_[cls];
    region->regionType = MSRegionType::MS_REGION_FULL;
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
    {
        std::lock_guard<std::mutex> lg(emptySweptLock_);
        emptySweptCache_.Prepend(region);
    }
    IncrementEmptySweptCount();
}

MSRegionInfo* CentralRegionPool::TakeEmptySweptRegionFromGlobalOnly()
{
    MSRegionInfo* region = nullptr;
    {
        std::lock_guard<std::mutex> lg(emptySweptLock_);
        region = emptySweptCache_.TakeHead();
    }
    if (region != nullptr) {
        DecrementEmptySweptCount();
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
    sweepStats_.Reset();

    // At this point MarkSetup has already stopped sweep and promoted any residual
    // unswept regions to swept. FlushAllAllocBuffers returns TL regions to swept.
    // O(1) per-bucket flip:
    //   partialSwept → partialUnswept (StealChainTo: head pointer transfer)
    //   fullSwept    ↔ fullUnswept    (Swap: head/count exchange)
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        buckets_[cls].FlipSweptToUnswept();
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
        sweepDone_.store(true, std::memory_order_release);
        finish();
        return {};
    }
    size_t maxRegions = GetBgSweepMaxRegionsPerSlice(remainingUnswept);
    uint64_t deadlineUs = t0 + GetBgSweepMaxTimeUs(remainingUnswept);
    size_t sweptInSlice = 0;
    bool pendingWork = false;
    size_t emptyBefore = sweepStats_.emptiedRegions.load(std::memory_order_relaxed);

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
                              sg, sweptInSlice, maxRegions, deadlineUs)) {
            pendingWork = !stopSweepRequested_.load(std::memory_order_acquire);
            return false;
        }
        if (!SweepUnsweptList(bucket, SizeClassBucket::UnsweptListKind::FULL,
                              sg, sweptInSlice, maxRegions, deadlineUs)) {
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

    size_t emptyInSlice = sweepStats_.emptiedRegions.load(std::memory_order_relaxed) - emptyBefore;

    if (!pendingWork && !stopSweepRequested_.load(std::memory_order_acquire)) {
        sweepDone_.store(true, std::memory_order_release);
    }
    finish();
    return BackgroundSweepStats{1, sweptInSlice, emptyInSlice, elapsed};
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

        MSRegionInfo* region = bucket.TakeUnswept(listKind);
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
                if (emptiedInSlice != nullptr) {
                    (*emptiedInSlice)++;
                }
            } else if (region->HasFreeSpace()) {
                sweepStats_.retainedRegions.fetch_add(1, std::memory_order_relaxed);
                region->regionType = MSRegionType::MS_REGION_PARTIAL;
                bucket.PushPartialSwept(region);
            } else {
                sweepStats_.retainedRegions.fetch_add(1, std::memory_order_relaxed);
                region->regionType = MSRegionType::MS_REGION_FULL;
                bucket.PushFullSwept(region);
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
    for (;;) {
        MSRegionInfo* region = bucket.TakeUnswept(listKind);
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
            bucket.PushPartialSwept(region);
            continue;
        }

        region->regionType = MSRegionType::MS_REGION_FULL;
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

bool CentralRegionPool::HasHungryAllocBuffer(uint8_t preferredCls) const
{
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
    for (AllocBufferMS* buffer : msAllocBuffers_) {
        if (buffer != nullptr && buffer->NeedsBackgroundFill(preferredCls)) {
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

// EagerSweepBatch — synchronous GC-thread sweep of a batch of unswept regions.
// Called from ConcurrentSweep() before handing off to the background thread,
// giving immediate reclaim under high allocation pressure.
// Uses the same CAS ownership protocol as BackgroundSweep / LazySweep.
MSEagerSweepBatchStats CentralRegionPool::EagerSweepBatch(uint64_t maxTimeUs)
{
    size_t unsweptBacklog = GetTotalUnsweptCount();
    if (maxTimeUs == 0) {
        return {};
    }
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    uint64_t deadlineUs = TimeUtil::MicroSeconds() + maxTimeUs;
    MSEagerSweepBatchStats stats;

    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        if (TimeUtil::MicroSeconds() >= deadlineUs) {
            break;
        }
        SizeClassBucket& bucket = buckets_[cls];
        // Sweep partialUnswept first (most likely to yield free slots).
        if (!SweepUnsweptList(bucket, SizeClassBucket::UnsweptListKind::PARTIAL,
                              sg, stats.sweptRegions, SIZE_MAX, deadlineUs,
                              &stats.emptiedRegions)) {
            break;
        }
        if (!SweepUnsweptList(bucket, SizeClassBucket::UnsweptListKind::FULL,
                              sg, stats.sweptRegions, SIZE_MAX, deadlineUs,
                              &stats.emptiedRegions)) {
            break;
        }
    }

    if (stats.sweptRegions > 0) {
        VLOG(REPORT, "[MS-GC] EagerSweepBatch: swept=%zu emptied=%zu unsweptBacklog=%zu (deadline=%zuus)",
             stats.sweptRegions, stats.emptiedRegions, unsweptBacklog, static_cast<size_t>(maxTimeUs));
    }
    return stats;
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
        msAllocBuffers_.erase(it);
    }
}

BackgroundFeedStats CentralRegionPool::BackgroundFillAllocCaches(uint8_t preferredCls)
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
            MSAllocCacheFillStats stats = buffer->BackgroundFillCache(preferredCls);
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
        VisitLargeRegionsLocked([&totalLive](MSRegionInfo* region) {
            if (region->IsMarked()) {
                totalLive += region->regionEnd - region->RegionStart();
            }
        });
    }

    return totalLive;
}

// ============================================================================
// VisitMarkedObjects: walk markBitmaps and call func() on every marked object.
// ============================================================================
// Same validity window as ComputeMarkedBytes: marking complete, no sweep
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

    // 2. Disable lazy sweep in GetRegion P2/P3
    skipLazySweep_.store(true, std::memory_order_release);

    // Pair with the seq_cst fence on the GetRegion lazy-sweep path: this makes the
    // skip-store / activeLazySweepCount_-load a sequentially consistent StoreLoad,
    // so any GetRegion thread that already incremented the counter is guaranteed
    // visible in step 4 below. Without it, ClearAllMarkBitmaps (called right after)
    // could clear a markBitmap while a lazy SweepRegion is still reading it.
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

    size_t unsweptBefore = GetTotalUnsweptCount();
    ForceFinishAllUnswept();
    size_t unsweptAfter = GetTotalUnsweptCount();
    size_t activeBytes = GetActiveMSRegionCount() * MS_REGION_SIZE;
    size_t emptySwept = GetEmptySweptRegionCount() * MS_REGION_SIZE;
    size_t largeCache = GetLargeHotCacheRegionCount() * MS_REGION_SIZE;
    size_t totalRetained = activeBytes + emptySwept + largeCache;
    VLOG(REPORT, "[MS-GC] StopAndWaitForAllSweep: forceSwept=%zu->%zu unswept regions, "
         "retained=%zuB activeBytes=%zuB emptySwept=%zuB largeCache=%zuB",
         unsweptBefore, unsweptAfter, totalRetained, activeBytes, emptySwept, largeCache);
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
    stopSweepRequested_.store(false, std::memory_order_release);
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
