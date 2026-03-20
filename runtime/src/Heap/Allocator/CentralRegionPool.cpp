// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "CentralRegionPool.h"
#include "AllocBufferMS.h"
#include "MSRegionManager.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include <cstdio>
#include <vector>

#include <thread>

namespace MapleRuntime {

// Global sweep generation counter (starts at 2 so that sg-2 == 0 is valid initial state)
std::atomic<uint32_t> CentralRegionPool::globalSweepGen_{2};

CentralRegionPool::CentralRegionPool(MSRegionManager* msRegionManager)
    : msRegionManager_(msRegionManager) {}

// ============================================================================
// GetRegion — four-priority Region acquisition (Go mcentral.cacheSpan equivalent)
// ============================================================================
MSRegionInfo* CentralRegionPool::GetRegion(uint8_t cls)
{
    SizeClassBucket& bucket = buckets_[cls];
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);

    // ---- Priority 1: partialSwept — lock-free CAS pop (O1, fastest) ----
    {
        MSRegionInfo* region = bucket.partialSwept.Pop();
        if (region != nullptr) {
            // Debug: validate region before returning
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
            // O3: preserve region type (BUMP from ResetToBump, or PARTIAL from sweep)
            return region;
        }
    }

    // ---- Priority 2: partialUnswept — needs in-place sweep, likely has free slots ----
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
                // P0: empty Region — return to MSRegionManager dirtyTree
                ReleaseRegionInternal(region);
                sweepStats_.releasedRegions.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            region->regionType = MSRegionType::MS_REGION_PARTIAL;
            return region;
        }
        // CAS failed: another thread is sweeping this Region, skip
    }

    // ---- Priority 3: fullUnswept — needs sweep, may produce free slots ----
    {
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
                    // P0: empty Region — return to MSRegionManager dirtyTree
                    ReleaseRegionInternal(region);
                    sweepStats_.releasedRegions.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (region->HasFreeSpace()) {
                    region->regionType = MSRegionType::MS_REGION_PARTIAL;
                    return region;
                }
                // Still full after sweep → move to fullSwept
                {
                    std::lock_guard<std::mutex> lg(bucket.lock);
                    bucket.fullSwept.Prepend(region);
                }
                continue;
            }
            // CAS failed: skip
        }
    }

    // ---- Priority 4: no cached Regions available, caller should GetFreshRegion ----
    return nullptr;
}

// ============================================================================
// GetFreshRegion — allocate a brand-new Region from MSRegionManager
// ============================================================================
MSRegionInfo* CentralRegionPool::GetFreshRegion(uint8_t cls)
{
    // MSRegionManager::TakeRegion allocates one MS_REGION_SIZE region
    // and returns the corresponding MSRegionInfo* directly (one-level).
    MSRegionInfo* msInfo = msRegionManager_->TakeRegion();
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

void CentralRegionPool::ReleaseRegion(MSRegionInfo* region, uint8_t /* cls */)
{
    ReleaseRegionInternal(region);
}

void CentralRegionPool::ReleaseRegionInternal(MSRegionInfo* region)
{
    region->ReleaseBitmaps();
    region->Reset();

    // Track total MS region bytes for GC trigger
    totalMSRegionBytes_.fetch_sub(MS_REGION_SIZE, std::memory_order_relaxed);

    // Return the underlying physical Region to MSRegionManager's free pool.
    // One-level: MSRegionManager directly manages the region via MSRegionInfo*.
    msRegionManager_->ReclaimRegion(region);
}

// ============================================================================
// GC operations
// ============================================================================

void CentralRegionPool::ForceFinishSweep()
{
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    size_t forceSweptCount = 0;
    size_t casFailCount = 0;

    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket& bucket = buckets_[cls];

        // Drain partialUnswept
        for (;;) {
            MSRegionInfo* region;
            {
                std::lock_guard<std::mutex> lg(bucket.lock);
                region = bucket.partialUnswept.TakeHead();
            }
            if (region == nullptr) {
                break;
            }
            uint32_t regionSG = region->GetSweepGen();
            if (region->CasSweepGen(sg - 2, sg - 1)) {
                region->SweepRegion();
                region->SetSweepGen(sg);
                forceSweptCount++;
                sweepStats_.forceSweptRegions.fetch_add(1, std::memory_order_relaxed);

                if (region->IsCompletelyFree()) {
                    // P0: empty Region — return to MSRegionManager dirtyTree
                    ReleaseRegionInternal(region);
                    sweepStats_.releasedRegions.fetch_add(1, std::memory_order_relaxed);
                } else if (region->HasFreeSpace()) {
                    region->regionType = MSRegionType::MS_REGION_PARTIAL;
                    bucket.partialSwept.Push(region);  // O1: lock-free
                } else {
                    region->regionType = MSRegionType::MS_REGION_FULL;
                    std::lock_guard<std::mutex> lg(bucket.lock);
                    bucket.fullSwept.Prepend(region);
                }
            } else {
                casFailCount++;
                LOG(RTLOG_ERROR, "[MS-GC] ForceFinishSweep CAS FAIL: region sweepGen=%u, expected=%u (sg=%u), cls=%u",
                    regionSG, sg - 2, sg, (unsigned)cls);
                // Put it back — don't lose the region
                bucket.partialSwept.Push(region);  // O1: lock-free
            }
        }

        // Drain fullUnswept
        for (;;) {
            MSRegionInfo* region;
            {
                std::lock_guard<std::mutex> lg(bucket.lock);
                region = bucket.fullUnswept.TakeHead();
            }
            if (region == nullptr) {
                break;
            }
            uint32_t regionSG = region->GetSweepGen();
            if (region->CasSweepGen(sg - 2, sg - 1)) {
                region->SweepRegion();
                region->SetSweepGen(sg);
                forceSweptCount++;
                sweepStats_.forceSweptRegions.fetch_add(1, std::memory_order_relaxed);

                if (region->IsCompletelyFree()) {
                    // P0: empty Region — return to MSRegionManager dirtyTree
                    ReleaseRegionInternal(region);
                    sweepStats_.releasedRegions.fetch_add(1, std::memory_order_relaxed);
                } else if (region->HasFreeSpace()) {
                    region->regionType = MSRegionType::MS_REGION_PARTIAL;
                    bucket.partialSwept.Push(region);  // O1: lock-free
                } else {
                    region->regionType = MSRegionType::MS_REGION_FULL;
                    std::lock_guard<std::mutex> lg(bucket.lock);
                    bucket.fullSwept.Prepend(region);
                }
            } else {
                casFailCount++;
                LOG(RTLOG_ERROR, "[MS-GC] ForceFinishSweep CAS FAIL: region sweepGen=%u, expected=%u (sg=%u), cls=%u",
                    regionSG, sg - 2, sg, (unsigned)cls);
                std::lock_guard<std::mutex> lg(bucket.lock);
                bucket.fullSwept.Prepend(region);
            }
        }
    }

    if (forceSweptCount > 0 || casFailCount > 0) {
        LOG(RTLOG_ERROR, "[MS-GC] ForceFinishSweep: swept=%zu, casFail=%zu, sg=%u",
            forceSweptCount, casFailCount, sg);
    }

    // Also sweep large regions — dead large objects hold contiguous pages
    // that must be returned to MSRegionManager so that subsequent large
    // allocations (TakeRegionForLarge) can find free aligned units.
    size_t largeFreedBytes = SweepLargeRegions();

    // Print current sweepStats to check if lazy/bg sweep did work
    LOG(RTLOG_ERROR, "[MS-GC] ForceFinishSweep stats: bg=%zu, lazy=%zu, force=%zu, released=%zu, largeFreed=%zu",
        sweepStats_.bgSweptRegions.load(std::memory_order_relaxed),
        sweepStats_.lazySweptRegions.load(std::memory_order_relaxed),
        sweepStats_.forceSweptRegions.load(std::memory_order_relaxed),
        sweepStats_.releasedRegions.load(std::memory_order_relaxed),
        largeFreedBytes);

    sweepDone_.store(true, std::memory_order_release);
}

void CentralRegionPool::FinishSweepBeforeMark()
{
    // First help drain sweep work that is still discoverable from the central
    // pool. This reduces how much of MarkSetup is spent in pure waiting.
    ForceFinishSweep();

    // Then wait for any background sweeper that had already claimed a region
    // before ForceFinishSweep() ran to fully quiesce.
    WaitForSweepDone();
}

void CentralRegionPool::SweepAndClassify(MSRegionInfo* region, uint32_t sg, SizeClassBucket& bucket)
{
    // CAS to claim sweep ownership (coordinate with BackgroundSweep / LazySweep)
    if (!region->CasSweepGen(sg - 2, sg - 1)) {
        return;  // Another thread is handling this Region
    }

    size_t freedBytes = region->SweepRegion();
    region->SetSweepGen(sg);
    sweepStats_.forceSweptRegions.fetch_add(1, std::memory_order_relaxed);
    sweepStats_.sweptByClass[region->sizeClass].fetch_add(1, std::memory_order_relaxed);
    sweepStats_.sweptFreedBytes.fetch_add(freedBytes, std::memory_order_relaxed);

    if (region->IsCompletelyFree()) {
        // P0: empty Region — return to MSRegionManager dirtyTree
        ReleaseRegionInternal(region);
        sweepStats_.releasedRegions.fetch_add(1, std::memory_order_relaxed);
        return;
    } else if (region->HasFreeSpace()) {
        region->regionType = MSRegionType::MS_REGION_PARTIAL;
        bucket.partialSwept.Push(region);  // O1: lock-free
    } else {
        region->regionType = MSRegionType::MS_REGION_FULL;
        std::lock_guard<std::mutex> lg(bucket.lock);
        bucket.fullSwept.Prepend(region);
    }
}

void CentralRegionPool::PrepareForSweep()
{
    // Reset sweep stats from previous cycle BEFORE flipping.
    // The stats accumulated in previous cycle should have been logged already.
    sweepStats_.Reset();

    // DEBUG: count regions in lists before swap
    size_t totalPartialSwept = 0, totalPartialUnswept = 0;
    size_t totalFullSwept = 0, totalFullUnswept = 0;

    // O(1) generation flip: drain lock-free partialSwept → partialUnswept, swap full lists.
    // ForceFinishSweep guarantees unswept lists are empty before this call.
    // PrepareForSweep runs during STW, so no concurrent Push/Pop on partialSwept.
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket& bucket = buckets_[cls];
        std::lock_guard<std::mutex> lg(bucket.lock);

        totalPartialSwept += bucket.partialSwept.Count();
        totalPartialUnswept += bucket.partialUnswept.Count();
        totalFullSwept += bucket.fullSwept.Count();
        totalFullUnswept += bucket.fullUnswept.Count();

        // O1: partialSwept is LockFreeRegionStack — drain into partialUnswept (MSRegionList)
        bucket.partialSwept.DrainTo(bucket.partialUnswept);
        bucket.fullSwept.Swap(bucket.fullUnswept);
    }

    LOG(RTLOG_ERROR, "[MS-GC] PrepareForSweep BEFORE swap: "
        "partialSwept=%zu, partialUnswept=%zu, fullSwept=%zu, fullUnswept=%zu",
        totalPartialSwept, totalPartialUnswept, totalFullSwept, totalFullUnswept);

    // Re-count AFTER swap
    size_t postPartialSwept = 0, postPartialUnswept = 0;
    size_t postFullSwept = 0, postFullUnswept = 0;
    for (uint8_t cls2 = 0; cls2 < kNumSizeClasses; cls2++) {
        SizeClassBucket& bucket2 = buckets_[cls2];
        std::lock_guard<std::mutex> lg2(bucket2.lock);
        postPartialSwept += bucket2.partialSwept.Count();
        postPartialUnswept += bucket2.partialUnswept.Count();
        postFullSwept += bucket2.fullSwept.Count();
        postFullUnswept += bucket2.fullUnswept.Count();
    }
    LOG(RTLOG_ERROR, "[MS-GC] PrepareForSweep AFTER swap: "
        "partialSwept=%zu, partialUnswept=%zu, fullSwept=%zu, fullUnswept=%zu",
        postPartialSwept, postPartialUnswept, postFullSwept, postFullUnswept);

    // Verify actual traversable count vs count_ for fullUnswept
    size_t actualFullUnsweptCount = 0;
    for (uint8_t cls3 = 0; cls3 < kNumSizeClasses; cls3++) {
        SizeClassBucket& bucket3 = buckets_[cls3];
        std::lock_guard<std::mutex> lg3(bucket3.lock);
        bucket3.fullUnswept.ForEach([&actualFullUnsweptCount](MSRegionInfo*) {
            actualFullUnsweptCount++;
        });
    }
    if (actualFullUnsweptCount != postFullUnswept) {
        LOG(RTLOG_ERROR, "[MS-GC] PrepareForSweep: fullUnswept COUNT MISMATCH! "
            "count_=%zu, actual traversal=%zu",
            postFullUnswept, actualFullUnsweptCount);
    }

    globalSweepGen_.fetch_add(2, std::memory_order_release);
    sweepDone_.store(false, std::memory_order_release);
}

// ============================================================================
// BackgroundSweep — called by gc-helper thread (FinalizerProcessor)
// ============================================================================

void CentralRegionPool::BackgroundSweep()
{
    bgSweepRunning_.store(true, std::memory_order_release);
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    uint64_t t0 = TimeUtil::MicroSeconds();

    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket& bucket = buckets_[cls];

        // Sweep partialUnswept
        SweepUnsweptList(bucket, bucket.partialUnswept, sg);

        // Sweep fullUnswept
        SweepUnsweptList(bucket, bucket.fullUnswept, sg);

        // Yield periodically to avoid starving other tasks on gc-helper
        if (cls % 6 == 5) {
            std::this_thread::yield();
        }
    }

    uint64_t elapsed = TimeUtil::MicroSeconds() - t0;
    sweepStats_.bgSweepTimeUs.store(elapsed, std::memory_order_relaxed);
    VLOG(REPORT, "[MS-GC] BackgroundSweep done: %zu regions swept, %zu released, %sus",
         sweepStats_.bgSweptRegions.load(std::memory_order_relaxed),
         sweepStats_.releasedRegions.load(std::memory_order_relaxed),
         Pretty(elapsed).Str());

    sweepDone_.store(true, std::memory_order_release);
    bgSweepRunning_.store(false, std::memory_order_release);
}

void CentralRegionPool::SweepUnsweptList(SizeClassBucket& bucket, MSRegionList& list, uint32_t sg)
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

        // CAS to claim sweep ownership (coordinate with Lazy Sweep / ForceFinishSweep)
        if (region->CasSweepGen(sg - 2, sg - 1)) {
            size_t freedBytes = region->SweepRegion();
            region->SetSweepGen(sg);
            sweepStats_.bgSweptRegions.fetch_add(1, std::memory_order_relaxed);
            sweepStats_.sweptByClass[region->sizeClass].fetch_add(1, std::memory_order_relaxed);
            sweepStats_.sweptFreedBytes.fetch_add(freedBytes, std::memory_order_relaxed);

            if (region->IsCompletelyFree()) {
                // P0: empty Region — return to MSRegionManager dirtyTree
                ReleaseRegionInternal(region);
                sweepStats_.releasedRegions.fetch_add(1, std::memory_order_relaxed);
            } else if (region->HasFreeSpace()) {
                region->regionType = MSRegionType::MS_REGION_PARTIAL;
                bucket.partialSwept.Push(region);  // O1: lock-free
            } else {
                region->regionType = MSRegionType::MS_REGION_FULL;
                std::lock_guard<std::mutex> lg(bucket.lock);
                bucket.fullSwept.Prepend(region);
            }
        }
        // CAS failed: Lazy Sweep or ForceFinish grabbed it, skip
    }
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

void CentralRegionPool::DumpPoolStats() const
{
    size_t partialSweptTotal = 0;
    size_t fullSweptTotal = 0;
    size_t partialUnsweptTotal = 0;
    size_t fullUnsweptTotal = 0;

    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        partialSweptTotal += buckets_[cls].partialSwept.Count();
        fullSweptTotal += buckets_[cls].fullSwept.Count();
        partialUnsweptTotal += buckets_[cls].partialUnswept.Count();
        fullUnsweptTotal += buckets_[cls].fullUnswept.Count();
    }

    size_t totalSmall = partialSweptTotal + fullSweptTotal + partialUnsweptTotal + fullUnsweptTotal;

    VLOG(REPORT, "[MS-GC]   CentralPool small (%u classes aggregated):", kNumSizeClasses);
    VLOG(REPORT, "[MS-GC]     partialSwept: %zu regions  fullSwept: %zu regions",
         partialSweptTotal, fullSweptTotal);
    VLOG(REPORT, "[MS-GC]     partialUnswept: %zu regions  fullUnswept: %zu regions",
         partialUnsweptTotal, fullUnsweptTotal);
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

void CentralRegionPool::ClearLargeMarks()
{
    std::lock_guard<std::mutex> lg(largeLock_);
    largeRegionList_.ForEach([](MSRegionInfo* region) {
        region->isMarked = 0;
    });
}

size_t CentralRegionPool::SweepLargeRegions()
{
    // Collect dead (unmarked) large regions into a local list, then release.
    // Two-pass to avoid holding largeLock_ during ReclaimRegion.
    std::vector<MSRegionInfo*> deadRegions;
    size_t freedBytes = 0;

    {
        std::lock_guard<std::mutex> lg(largeLock_);
        // Iterate and remove dead regions from the list
        int32_t idx = largeRegionList_.IsEmpty() ? MSRegionInfo::INVALID_IDX : 0;
        // We need to iterate using ForEach and collect dead ones
        std::vector<MSRegionInfo*> toRemove;
        largeRegionList_.ForEach([&toRemove](MSRegionInfo* region) {
            if (region->isMarked == 0) {
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
        freedBytes += regionBytes;

        // Reset span MSRegionInfo entries before resetting the base entry.
        // RegisterLargeObjectMS initializes span entries with regionType=LARGE
        // and reserved=baseIdx; we must clear them to avoid stale lookups.
        size_t spanCount = (regionBytes + MS_REGION_SIZE - 1) / MS_REGION_SIZE;
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

    return freedBytes;
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
    std::lock_guard<std::mutex> lg(msAllocBufferLock_);
    for (AllocBufferMS* buffer : msAllocBuffers_) {
        if (buffer != nullptr) {
            buffer->FlushAll();
        }
    }
}

void CentralRegionPool::BackgroundFillAllocCaches()
{
    std::lock_guard<std::mutex> lg(msAllocBufferLock_);
    for (AllocBufferMS* buffer : msAllocBuffers_) {
        if (buffer != nullptr) {
            buffer->BackgroundFillCache();
        }
    }
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
            if (region->isMarked) {
                totalLive += region->regionEnd - region->RegionStart();
            }
        });
    }

    return totalLive;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
