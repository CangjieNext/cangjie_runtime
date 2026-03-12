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

    // ---- Priority 1: partialSwept — already swept, has free slots (fastest) ----
    {
        std::lock_guard<std::mutex> lg(bucket.lock);
        MSRegionInfo* region = bucket.partialSwept.TakeHead();
        if (region != nullptr) {
            region->regionType = MSRegionType::MS_REGION_PARTIAL;
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
            region->SweepRegion();
            region->SetSweepGen(sg);
            sweepStats_.lazySweptRegions.fetch_add(1, std::memory_order_relaxed);

            if (region->IsCompletelyFree()) {
                sweepStats_.releasedRegions.fetch_add(1, std::memory_order_relaxed);
                ReleaseRegionInternal(region);
                continue;  // All dead — release and try next
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
                region->SweepRegion();
                region->SetSweepGen(sg);
                sweepStats_.lazySweptRegions.fetch_add(1, std::memory_order_relaxed);

                if (region->IsCompletelyFree()) {
                    sweepStats_.releasedRegions.fetch_add(1, std::memory_order_relaxed);
                    ReleaseRegionInternal(region);
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

    // Initialize for the requested size class (sets bitmaps, slot counts, state)
    msInfo->InitForSizeClass(cls);

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
    std::lock_guard<std::mutex> lg(bucket.lock);
    region->regionType = MSRegionType::MS_REGION_PARTIAL;
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    if (region->GetSweepGen() == sg) {
        bucket.partialSwept.Prepend(region);
    } else {
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
            SweepAndClassify(region, sg, bucket);
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
            SweepAndClassify(region, sg, bucket);
        }
    }

    sweepDone_.store(true, std::memory_order_release);
}

void CentralRegionPool::SweepAndClassify(MSRegionInfo* region, uint32_t sg, SizeClassBucket& bucket)
{
    // CAS to claim sweep ownership (coordinate with BackgroundSweep / LazySweep)
    if (!region->CasSweepGen(sg - 2, sg - 1)) {
        return;  // Another thread is handling this Region
    }

    region->SweepRegion();
    region->SetSweepGen(sg);
    sweepStats_.forceSweptRegions.fetch_add(1, std::memory_order_relaxed);

    if (region->IsCompletelyFree()) {
        sweepStats_.releasedRegions.fetch_add(1, std::memory_order_relaxed);
        ReleaseRegionInternal(region);
    } else if (region->HasFreeSpace()) {
        std::lock_guard<std::mutex> lg(bucket.lock);
        bucket.partialSwept.Prepend(region);
    } else {
        std::lock_guard<std::mutex> lg(bucket.lock);
        bucket.fullSwept.Prepend(region);
    }
}

void CentralRegionPool::PrepareForSweep()
{
    // Reset sweep stats from previous cycle BEFORE flipping.
    // The stats accumulated in previous cycle should have been logged already.
    sweepStats_.Reset();

    // O(1) generation flip: swap swept ↔ unswept lists, then bump sweepGen.
    // ForceFinishSweep guarantees unswept lists are empty before this call,
    // so swap is safe and equivalent to Go mcentral partial/full index flip.
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket& bucket = buckets_[cls];
        std::lock_guard<std::mutex> lg(bucket.lock);

        bucket.partialSwept.Swap(bucket.partialUnswept);
        bucket.fullSwept.Swap(bucket.fullUnswept);
    }

    globalSweepGen_.fetch_add(2, std::memory_order_release);
    sweepDone_.store(false, std::memory_order_release);
}

// ============================================================================
// BackgroundSweep — called by gc-helper thread (FinalizerProcessor)
// ============================================================================

void CentralRegionPool::BackgroundSweep()
{
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);

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

    sweepDone_.store(true, std::memory_order_release);
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
            region->SweepRegion();
            region->SetSweepGen(sg);
            sweepStats_.bgSweptRegions.fetch_add(1, std::memory_order_relaxed);

            if (region->IsCompletelyFree()) {
                sweepStats_.releasedRegions.fetch_add(1, std::memory_order_relaxed);
                ReleaseRegionInternal(region);
            } else if (region->HasFreeSpace()) {
                std::lock_guard<std::mutex> lg(bucket.lock);
                bucket.partialSwept.Prepend(region);
            } else {
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

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
