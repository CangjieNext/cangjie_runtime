// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_CENTRAL_REGION_POOL_H
#define MRT_CENTRAL_REGION_POOL_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <mutex>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <thread>

#include "MSRegionInfo.h"
#include "SizeClass.h"

namespace MapleRuntime {

class MSRegionManager;
class AllocBufferMS;

// Sweep statistics accumulated across a GC cycle, persisted until next GC.
struct MSSweepStats {
    std::atomic<size_t> bgSweptRegions{0};    // regions swept by background sweep
    std::atomic<size_t> lazySweptRegions{0};  // regions swept by lazy sweep (GetRegion)
    std::atomic<size_t> forceSweptRegions{0}; // regions swept by ForceFinishSweep
    std::atomic<size_t> sweptFreedBytes{0};   // total bytes freed by small-object sweep
    std::atomic<size_t> releasedRegions{0};   // completely-free regions returned to OS
    std::atomic<uint64_t> bgSweepTimeUs{0};   // BackgroundSweep wall-clock time (us)

    // O3: per-sizeClass sweep region count for fragmentation diagnostics
    std::atomic<size_t> sweptByClass[kNumSizeClasses]{};

    void Reset() {
        bgSweptRegions.store(0, std::memory_order_relaxed);
        lazySweptRegions.store(0, std::memory_order_relaxed);
        forceSweptRegions.store(0, std::memory_order_relaxed);
        sweptFreedBytes.store(0, std::memory_order_relaxed);
        releasedRegions.store(0, std::memory_order_relaxed);
        bgSweepTimeUs.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < kNumSizeClasses; i++) {
            sweptByClass[i].store(0, std::memory_order_relaxed);
        }
    }

    size_t TotalSweptRegions() const {
        return bgSweptRegions.load(std::memory_order_relaxed) +
               lazySweptRegions.load(std::memory_order_relaxed) +
               forceSweptRegions.load(std::memory_order_relaxed);
    }
};

// ============================================================================
// MSRegionList: lightweight intrusive doubly-linked list over MSRegionInfo
// ============================================================================
// Uses MSRegionInfo::nextRegionIdx / prevRegionIdx for linking.
// Caller must hold the bucket lock; no internal locking.
//
class MSRegionList {
public:
    MSRegionList() = default;

    void Prepend(MSRegionInfo* region)
    {
        int32_t idx = static_cast<int32_t>(MSRegionInfoArray::GetRegionIdx(region));
        region->prevRegionIdx = MSRegionInfo::INVALID_IDX;
        region->nextRegionIdx = headIdx_;
        if (headIdx_ != MSRegionInfo::INVALID_IDX) {
            MSRegionInfoArray::GetRegionInfoByIdx(headIdx_)->prevRegionIdx = idx;
        }
        headIdx_ = idx;
        count_++;
    }

    MSRegionInfo* TakeHead()
    {
        if (headIdx_ == MSRegionInfo::INVALID_IDX) {
            return nullptr;
        }
        MSRegionInfo* head = MSRegionInfoArray::GetRegionInfoByIdx(headIdx_);
        int32_t nextIdx = head->nextRegionIdx;
        if (nextIdx != MSRegionInfo::INVALID_IDX) {
            MSRegionInfoArray::GetRegionInfoByIdx(nextIdx)->prevRegionIdx = MSRegionInfo::INVALID_IDX;
        }
        headIdx_ = nextIdx;
        head->nextRegionIdx = MSRegionInfo::INVALID_IDX;
        head->prevRegionIdx = MSRegionInfo::INVALID_IDX;
        count_--;
        return head;
    }

    void Remove(MSRegionInfo* region)
    {
        int32_t prevIdx = region->prevRegionIdx;
        int32_t nextIdx = region->nextRegionIdx;

        if (prevIdx != MSRegionInfo::INVALID_IDX) {
            MSRegionInfoArray::GetRegionInfoByIdx(prevIdx)->nextRegionIdx = nextIdx;
        } else {
            headIdx_ = nextIdx;
        }

        if (nextIdx != MSRegionInfo::INVALID_IDX) {
            MSRegionInfoArray::GetRegionInfoByIdx(nextIdx)->prevRegionIdx = prevIdx;
        }

        region->nextRegionIdx = MSRegionInfo::INVALID_IDX;
        region->prevRegionIdx = MSRegionInfo::INVALID_IDX;
        count_--;
    }

    bool IsEmpty() const { return headIdx_ == MSRegionInfo::INVALID_IDX; }
    size_t Count() const { return count_; }

    void Swap(MSRegionList& other)
    {
        std::swap(headIdx_, other.headIdx_);
        std::swap(count_, other.count_);
    }

    // O(1) accept a single-linked chain (nextRegionIdx only, prev NOT set).
    // List MUST be empty. Safe when consumers only use TakeHead (which fixes prev lazily).
    void AcceptSingleLinkedChain(int32_t headIdx, size_t count)
    {
        headIdx_ = headIdx;
        count_ = count;
    }

    // Iterate all elements, calling visitor(MSRegionInfo*) for each.
    // Visitor may NOT modify the list (use TakeHead loop for drain).
    template<typename Visitor>
    void ForEach(Visitor&& visitor) const
    {
        int32_t idx = headIdx_;
        while (idx != MSRegionInfo::INVALID_IDX) {
            MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoByIdx(idx);
            int32_t nextIdx = info->nextRegionIdx;
            visitor(info);
            idx = nextIdx;
        }
    }

private:
    int32_t headIdx_ = MSRegionInfo::INVALID_IDX;
    size_t  count_   = 0;
};

// ============================================================================
// LockFreeRegionStack: CAS-based lock-free stack for partialSwept (O1)
// ============================================================================
// Single-linked stack using MSRegionInfo::nextRegionIdx.
// No ABA risk: a region is exclusively owned by one thread after Pop,
// so the same region cannot be concurrently Pushed by another thread.
//
class LockFreeRegionStack {
public:
    LockFreeRegionStack() = default;

    void Push(MSRegionInfo* region)
    {
        int32_t idx = static_cast<int32_t>(MSRegionInfoArray::GetRegionIdx(region));
        int32_t oldHead = head_.load(std::memory_order_relaxed);
        do {
            region->nextRegionIdx = oldHead;
        } while (!head_.compare_exchange_weak(oldHead, idx,
                     std::memory_order_release, std::memory_order_relaxed));
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    MSRegionInfo* Pop()
    {
        int32_t oldHead = head_.load(std::memory_order_acquire);
        MSRegionInfo* headRegion;
        do {
            if (oldHead == MSRegionInfo::INVALID_IDX) {
                return nullptr;
            }
            headRegion = MSRegionInfoArray::GetRegionInfoByIdx(oldHead);
        } while (!head_.compare_exchange_weak(oldHead, headRegion->nextRegionIdx,
                     std::memory_order_release, std::memory_order_relaxed));
        headRegion->nextRegionIdx = MSRegionInfo::INVALID_IDX;
        headRegion->prevRegionIdx = MSRegionInfo::INVALID_IDX;
        count_.fetch_sub(1, std::memory_order_relaxed);
        return headRegion;
    }

    bool IsEmpty() const { return head_.load(std::memory_order_acquire) == MSRegionInfo::INVALID_IDX; }
    size_t Count() const { return count_.load(std::memory_order_relaxed); }

    // O(n) drain all elements into an MSRegionList (under external synchronization, e.g. STW).
    void DrainTo(MSRegionList& dest)
    {
        int32_t idx = head_.exchange(MSRegionInfo::INVALID_IDX, std::memory_order_acquire);
        size_t drained = 0;
        while (idx != MSRegionInfo::INVALID_IDX) {
            MSRegionInfo* region = MSRegionInfoArray::GetRegionInfoByIdx(idx);
            int32_t next = region->nextRegionIdx;
            region->nextRegionIdx = MSRegionInfo::INVALID_IDX;
            region->prevRegionIdx = MSRegionInfo::INVALID_IDX;
            dest.Prepend(region);
            idx = next;
            drained++;
        }
        count_.fetch_sub(drained, std::memory_order_relaxed);
    }

    // O(1) transfer entire chain to MSRegionList. Must be called during STW.
    // Destination MUST be empty. Prev pointers are NOT fixed — safe when
    // consumers only use TakeHead (which fixes prev lazily).
    void StealChainTo(MSRegionList& dest)
    {
        int32_t h = head_.load(std::memory_order_relaxed);
        size_t cnt = count_.load(std::memory_order_relaxed);
        head_.store(MSRegionInfo::INVALID_IDX, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
        dest.AcceptSingleLinkedChain(h, cnt);
    }

    // Unsafe iteration for GC (STW only). Visitor must NOT modify the stack.
    template<typename Visitor>
    void ForEachUnsafe(Visitor&& visitor) const
    {
        int32_t idx = head_.load(std::memory_order_acquire);
        while (idx != MSRegionInfo::INVALID_IDX) {
            MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoByIdx(idx);
            int32_t next = info->nextRegionIdx;
            visitor(info);
            idx = next;
        }
    }

private:
    std::atomic<int32_t> head_{MSRegionInfo::INVALID_IDX};
    std::atomic<size_t>  count_{0};
};

// ============================================================================
// CentralRegionPool: Go mcentral-equivalent per-size-class Region pool
// ============================================================================
//
// Four-list design per size class (aligned with Go 1.16+ mcentral):
//   partialSwept   — has free slots, already swept (fastest path)
//   partialUnswept — has free slots, needs in-place sweep
//   fullSwept      — no free slots, already swept
//   fullUnswept    — no free slots, needs sweep
//
// Lazy Sweep: sweep is deferred to allocation time. When a thread needs
// a Region, it pops from unswept lists and sweeps in-place (CAS ownership).
//
// Generation flip: PrepareForSweep() increments globalSweepGen_ by 2,
// making all "swept" Regions logically "unswept" — O(1), no list moves.
//
class CentralRegionPool {
public:
    explicit CentralRegionPool(MSRegionManager* msRegionManager);
    ~CentralRegionPool() = default;

    // ===== Thread-local Region acquisition =====

    // Get a Region with free slots for the given size class.
    // Priority: partialSwept → partialUnswept (sweep) → fullUnswept (sweep) → fresh.
    MSRegionInfo* GetRegion(uint8_t cls);

    // Get a brand-new Region from RegionManager, initialized for the given size class.
    MSRegionInfo* GetFreshRegion(uint8_t cls);

    // ===== Thread-local Region return =====

    void ReturnPartialRegion(MSRegionInfo* region, uint8_t cls);
    void ReturnFullRegion(MSRegionInfo* region, uint8_t cls);
    void ReleaseRegion(MSRegionInfo* region, uint8_t cls);

    // ===== GC operations =====

    // Force-finish all remaining sweeps before starting new GC cycle.
    // Corresponds to Go's finishsweep_m(). This actively drains work that is
    // still discoverable from the unswept lists, but does not by itself prove
    // that an already-running background sweeper has fully quiesced.
    void ForceFinishSweep();

    // Help complete the previous cycle's sweep work before the next mark setup.
    // The current thread first drains remaining unclaimed unswept work, then
    // waits for any already-running background sweeper to fully quiesce.
    void FinishSweepBeforeMark();

    // Generation flip: globalSweepGen_ += 2.
    // All swept Regions logically become unswept.
    void PrepareForSweep();

    // Background sweep: iterate all size classes, sweep unswept Regions.
    // Thread-safe (CAS ownership). Called by gc-helper thread.
    void BackgroundSweep();

    // Check whether previous-cycle sweep work is globally quiescent.
    bool IsSweepDone() const
    {
        return sweepDone_.load(std::memory_order_acquire) &&
               !bgSweepRunning_.load(std::memory_order_acquire);
    }

    // Visit all Regions across all size classes (for GC root enumeration).
    template<typename Visitor>
    void VisitAllRegions(Visitor&& visitor);

    // ===== Monitoring =====
    size_t GetTotalPartialCount() const;
    size_t GetTotalFullCount() const;

    // ===== DFX: Sweep statistics (persisted across GC cycle) =====
    MSSweepStats& GetSweepStats() { return sweepStats_; }
    const MSSweepStats& GetSweepStats() const { return sweepStats_; }

    // Dump CentralRegionPool 4-list statistics (block ⑤ in DFX design)
    void DumpPoolStats() const;

    static uint32_t GetGlobalSweepGen()
    {
        return globalSweepGen_.load(std::memory_order_acquire);
    }

    // Total bytes of MS regions in use (for GC trigger threshold check)
    size_t GetTotalMSRegionBytes() const
    {
        return totalMSRegionBytes_.load(std::memory_order_relaxed);
    }

    // Compute true live bytes by walking markBitmaps of all regions.
    // Must be called after marking completes and BEFORE PrepareForSweep.
    size_t ComputeMarkedBytes() const;

    // ===== Large region management (Tier 1: in-heap large objects) =====

    // Register a large MSRegionInfo (MS_REGION_LARGE) for GC tracking.
    void RegisterLargeRegion(MSRegionInfo* region);

    // Eager sweep: reclaim unmarked large regions, return freed bytes.
    // Called after MarkTerminate / in PostMarkTermination.
    size_t SweepLargeRegions();

    // Wait until previous-cycle sweep work is globally quiescent.
    // Actively helps drain remaining sweep work (via ForceFinishSweep)
    // instead of spinning idle, then waits for any in-flight background
    // sweeper to finish.
    void WaitForSweepDone();

    // ===== Pre-STW bitmap clearing (MarkSetup fast path) =====

    // Stop background sweep and disable lazy sweep, wait for all in-flight
    // sweeps (bg + lazy) to fully quiesce. Must be called before clearing
    // markBitmaps to avoid data races with SweepRegion reading markBitmap.
    void StopAndWaitForAllSweep();

    // Clear markBitmaps for all active small/medium regions in MSRegionInfoArray.
    // CAS-stamps unswept regions (sg-2 -> sg) to prevent future lazy sweep.
    // Must be called after StopAndWaitForAllSweep().
    void ClearAllMarkBitmaps();

    // Drain unswept lists into swept lists after stamping all regions as swept.
    void ReclassifyUnsweptToSwept();

    // Re-enable lazy sweep after MarkSetup bitmap clearing is complete.
    void ReenableLazySweep();

    // Adjust totalMSRegionBytes_ for large region tracking.
    void AddLargeRegionBytes(size_t bytes)
    {
        totalMSRegionBytes_.fetch_add(bytes, std::memory_order_relaxed);
    }
    void SubLargeRegionBytes(size_t bytes)
    {
        totalMSRegionBytes_.fetch_sub(bytes, std::memory_order_relaxed);
    }

private:
    struct SizeClassBucket {
        std::mutex lock;              // protects fullSwept, partialUnswept, fullUnswept only
        LockFreeRegionStack partialSwept;  // O1: lock-free CAS stack for P1 hot path
        MSRegionList fullSwept;
        MSRegionList partialUnswept;
        MSRegionList fullUnswept;
    };

    void ReleaseRegionInternal(MSRegionInfo* region);
    void SweepAndClassify(MSRegionInfo* region, uint32_t sg, SizeClassBucket& bucket);
    void SweepUnsweptList(SizeClassBucket& bucket, MSRegionList& list, uint32_t sg);

    static std::atomic<uint32_t> globalSweepGen_;
    std::atomic<bool> sweepDone_{true};
    std::atomic<bool> bgSweepRunning_{false};
    std::atomic<bool> stopSweepRequested_{false};    // Signal bg sweep to stop early
    std::atomic<bool> skipLazySweep_{false};          // Disable lazy sweep in GetRegion P2/P3
    std::atomic<size_t> activeLazySweepCount_{0};     // In-flight lazy sweep counter
    MSSweepStats sweepStats_;

    // Total bytes of MS regions currently committed (GetFreshRegion +, ReleaseRegion -).
    // This is a region-level metric, NOT object-level:
    //   - Includes live objects, dead objects in unswept regions, and intra-region fragmentation.
    //   - After GC ends but before sweep completes, it remains inflated.
    //   - Only decreases when sweep reclaims regions back to MSRegionManager.
    // The GC trigger (MSRegionManager::TakeRegion) uses the O1 effective-allocation
    // formula to compensate for this semantic gap vs trueLiveBytes.
    std::atomic<size_t> totalMSRegionBytes_{0};

    SizeClassBucket buckets_[kNumSizeClasses];
    MSRegionManager*  msRegionManager_;

    // Large region tracking (Tier 1: in-heap large objects, MS_REGION_LARGE)
    std::mutex largeLock_;
    MSRegionList largeRegionList_;

    // ===== Per-thread AllocBufferMS registry =====
    // Every thread lazily creates an AllocBufferMS and registers it here.
    // GC STW uses FlushAllAllocBuffers() to drain all thread-local Regions.
public:
    void RegisterAllocBufferMS(AllocBufferMS* buffer);
    void RemoveAllocBufferMS(AllocBufferMS* buffer);
    void FlushAllAllocBuffers();

    // O9: Background fill of hungry per-thread alloc caches.
    // Called by gc-helper thread. Iterates all AllocBufferMS and fills
    // cachedRegion_ slots for size classes with high miss pressure.
    void BackgroundFillAllocCaches();

    // Visit all registered AllocBufferMS instances (under lock)
    template<typename Visitor>
    void VisitAllocBufferMS(Visitor&& visitor)
    {
        std::lock_guard<std::mutex> lg(msAllocBufferLock_);
        for (AllocBufferMS* buffer : msAllocBuffers_) {
            if (buffer != nullptr) {
                visitor(*buffer);
            }
        }
    }
private:
    std::mutex msAllocBufferLock_;
    std::vector<AllocBufferMS*> msAllocBuffers_;
};

// ===== Template implementation =====

template<typename Visitor>
void CentralRegionPool::VisitAllRegions(Visitor&& visitor)
{
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SizeClassBucket& bucket = buckets_[cls];
        // partialSwept is lock-free; ForEachUnsafe is safe during STW
        bucket.partialSwept.ForEachUnsafe(visitor);
        std::lock_guard<std::mutex> lg(bucket.lock);
        bucket.fullSwept.ForEach(visitor);
        bucket.partialUnswept.ForEach(visitor);
        bucket.fullUnswept.ForEach(visitor);
    }
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_CENTRAL_REGION_POOL_H
