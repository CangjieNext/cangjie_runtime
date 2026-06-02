// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_CENTRAL_REGION_POOL_H
#define MRT_MS_CENTRAL_REGION_POOL_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <array>
#include <mutex>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <thread>
#include <functional>

#include "Heap/Allocator/Allocator.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSSizeClass.h"

namespace MapleRuntime {

class MSRegionManager;
class AllocBufferMS;
class BaseObject;

enum class MSRegionAcquireSource : uint8_t {
    NONE = 0,
    PARTIAL_SWEPT,
    PARTIAL_UNSWEPT,
    FULL_UNSWEPT,
    EMPTY_REBIND,
};

// Sweep statistics accumulated across a GC cycle, persisted until next GC.
struct MSSweepStats {
    std::atomic<size_t> bgSweptRegions{0};    // regions swept by background sweep
    std::atomic<size_t> lazySweptRegions{0};  // regions swept by lazy sweep (GetRegion)
    std::atomic<size_t> sweptFreedBytes{0};   // total bytes freed by small-object sweep
    std::atomic<size_t> emptiedRegions{0};    // completely-free small regions reset and returned to swept cache
    std::atomic<size_t> retainedRegions{0};   // swept small regions that still remain active
    std::atomic<uint64_t> bgSweepTimeUs{0};   // BackgroundSweep wall-clock time (us)

    // O3: per-sizeClass sweep region count for fragmentation diagnostics
    std::atomic<size_t> sweptByClass[kNumSizeClasses]{};

    void Reset() {
        bgSweptRegions.store(0, std::memory_order_relaxed);
        lazySweptRegions.store(0, std::memory_order_relaxed);
        sweptFreedBytes.store(0, std::memory_order_relaxed);
        emptiedRegions.store(0, std::memory_order_relaxed);
        retainedRegions.store(0, std::memory_order_relaxed);
        bgSweepTimeUs.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < kNumSizeClasses; i++) {
            sweptByClass[i].store(0, std::memory_order_relaxed);
        }
    }

    size_t TotalSweptRegions() const {
        return bgSweptRegions.load(std::memory_order_relaxed) +
               lazySweptRegions.load(std::memory_order_relaxed);
    }
};

struct MSLargeSweepStats {
    size_t objectCount = 0;
    size_t regionCount = 0;
    size_t freedBytes = 0;
    size_t cachedObjectCount = 0;  // Dead large objects placed into hot cache (not immediately reclaimed).
    size_t cachedRegionCount = 0;  // Region spans placed into hot cache.
    size_t cachedBytes = 0;        // Bytes placed into hot cache.
};

struct MSEagerSweepBatchStats {
    size_t sweptRegions = 0;
    size_t emptiedRegions = 0;
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
    struct BucketPressure {
        size_t partialSwept = 0;
        size_t partialUnswept = 0;
        size_t fullSwept = 0;
        size_t fullUnswept = 0;

        size_t ReadyCount() const
        {
            return partialSwept + partialUnswept;
        }

        size_t UnsweptCount() const
        {
            return partialUnswept + fullUnswept;
        }
    };

    explicit CentralRegionPool(MSRegionManager* msRegionManager);
    ~CentralRegionPool() = default;

    // ===== Thread-local Region acquisition =====

    // Get a Region with free slots for the given size class.
    // Priority: partialSwept → partialUnswept (lazy sweep) → fullUnswept (lazy sweep)
    //        → emptySweptCache(rebind) → fresh.
    MSRegionInfo* GetRegion(uint8_t cls, MSRegionAcquireSource* source = nullptr);

    // Get a fresh mutator Region from RegionManager after central lists miss.
    // This path skips releasedTree and goes directly to bump/grow.
    MSRegionInfo* GetFreshRegion(uint8_t cls);

    // ===== Thread-local Region return =====

    void ReturnPartialRegion(MSRegionInfo* region, uint8_t cls);
    void ReturnFullRegion(MSRegionInfo* region, uint8_t cls);
    void ReturnEmptyRegion(MSRegionInfo* region);

    // ===== GC operations =====

    // Generation flip: globalSweepGen_ += 2.
    // All swept Regions logically become unswept.
    void PrepareForSweep();

    // Background sweep: iterate all size classes, sweep one slice of unswept Regions.
    // Thread-safe (CAS ownership). Called by gc-helper thread.
    BackgroundSweepStats BackgroundSweep(uint8_t preferredCls = kNumSizeClasses);

    bool HasPendingSweepWork() const
    {
        return !stopSweepRequested_.load(std::memory_order_acquire) &&
               GetTotalUnsweptCount() > 0;
    }

    // Reclaim up to maxRegions empty regions from emptySweptCache into released state.
    // Passing a very large maxRegions value releases all currently cached empty regions.
    size_t ReclaimEmptyRegion(size_t maxRegions);

    // Check whether previous-cycle sweep work is globally quiescent.
    bool IsSweepDone() const
    {
        return sweepDone_.load(std::memory_order_acquire) &&
               !bgSweepRunning_.load(std::memory_order_acquire) &&
               activeLazySweepCount_.load(std::memory_order_acquire) == 0;
    }

    // Visit all Regions across all size classes (for GC root enumeration).
    template<typename Visitor>
    void VisitAllRegions(Visitor&& visitor);

    // ===== Monitoring =====
    size_t GetTotalPartialCount() const;
    size_t GetTotalFullCount() const;
    size_t GetTotalUnsweptCount() const;
    size_t GetTotalSweptCount() const;
    size_t GetTotalPartialSweptCount() const;
    size_t GetTotalFullUnsweptCount() const;
    size_t GetLargeRegionCount() const;
    size_t GetLargeRegionSpanCount() const;
    size_t CountTLActiveRegions() const;
    size_t GetAllocBufferCount() const;
    size_t GetAllAllocBufferRegionCount() const;
    size_t GetAllAllocBufferActiveRegionCount() const;
    size_t GetAllAllocBufferCachedRegionCount() const;
    bool HasHungryAllocBuffer(uint8_t preferredCls = UINT8_MAX) const;
    BucketPressure GetBucketPressure(uint8_t cls) const;
    void MaybeNotifySweep(uint8_t cls) const;
    // Swept-retained region count: partialSwept + fullSwept buckets + large region spans.
    // Excludes unswept regions; used by MaybeNotifyMark to gate the next GC on the
    // post-sweep live footprint rather than the transient active-region total.
    size_t GetSweptMSRegionCount() const;
    size_t GetActiveMSRegionCount() const;
    size_t GetEmptySweptRegionCount() const;
    size_t GetLargeHotCacheRegionCount() const;

    // Step 11: inline reclaim support — used by MSRegionManager::InlineReclaimForBalance
    MSRegionInfo* TakeEmptySweptRegionForReclaim();

    // ===== DFX: Sweep statistics (persisted across GC cycle) =====
    MSSweepStats& GetSweepStats() { return sweepStats_; }
    const MSSweepStats& GetSweepStats() const { return sweepStats_; }

    // Dump CentralRegionPool 4-list statistics (block ⑤ in DFX design)
    void DumpPoolStats() const;

    static uint32_t GetGlobalSweepGen()
    {
        return globalSweepGen_.load(std::memory_order_acquire);
    }

    // Compute true live bytes by walking markBitmaps of all regions.
    // Must be called after marking completes and BEFORE PrepareForSweep.
    size_t ComputeMarkedBytes() const;

    // Diagnostic: visit every currently-marked object (small/medium + large)
    // by walking markBitmaps. Must be called in the same window as
    // ComputeMarkedBytes (marking complete, no sweep started). Used by the
    // mark-completeness verifier to detect live objects that were missed.
    void VisitMarkedObjects(const std::function<void(BaseObject*)>& func) const;

    // ===== Large region management (Tier 1: in-heap large objects) =====

    // Register a large MSRegionInfo (MS_REGION_LARGE) for GC tracking.
    void RegisterLargeRegion(MSRegionInfo* region);

    // Eager sweep: reclaim unmarked large regions, return freed bytes.
    // Called after MarkTerminate / in PostMarkTermination.
    MSLargeSweepStats SweepLargeRegions();

    // Large object hot cache: try to cache a freshly-freed large region so
    // its physical pages remain resident for fast reuse on the next allocation.
    // Returns true if accepted; false if the cache bucket is full (caller should
    // call ReclaimRegionLarge instead).
    bool TryPutLargeHotCache(uintptr_t regionStart, size_t regionBytes);

    // Pop a cached entry matching regionBytes from the hot cache.
    // Returns true and sets regionStart if found; false if miss.
    bool TryTakeLargeHotCache(size_t regionBytes, uintptr_t& regionStart);

    // Reclaim large-hot-cache entries back to MSRegionManager (releasedTree),
    // largest entries first, up to maxBytes (SIZE_MAX = drain all).
    // This is the single low-level release primitive for the large hot cache;
    // flush policy lives in RegionSpaceMS. Returns reclaimed bytes.
    size_t ReclaimLargeHotCache(size_t maxBytes);

    // Returns true if the large hot cache is non-empty (lock-free).
    bool HasLargeFreeCache() const;

    // Eager synchronous sweep of small regions: GC thread sweeps unswept regions
    // across all size classes until the configured wall-time budget is exhausted.
    // Called from ConcurrentSweep() to give immediate reclaim under pressure.
    MSEagerSweepBatchStats EagerSweepBatch(uint64_t maxTimeUs);

    // ===== Pre-STW bitmap clearing (MarkSetup fast path) =====

    // Stop background sweep and disable lazy sweep, then wait for all
    // in-flight sweeps (bg + lazy) to fully quiesce. Any remaining central
    // unswept backlog is synchronously finished before bitmap clearing, so the
    // next mark setup does not reclassify central unswept lists without a real sweep.
    // Must be called before clearing markBitmaps to avoid data races with
    // SweepRegion reading markBitmap.
    void StopAndWaitForAllSweep();

    // Clear markBitmaps for all active small/medium regions in MSRegionInfoArray.
    // CAS-stamps any residual unswept regions (sg-2 -> sg) to prevent future
    // lazy sweep from observing stale markBitmaps.
    // Must be called after StopAndWaitForAllSweep().
    void ClearAllMarkBitmaps();

    // Drain any residual unswept lists into swept lists after stamping regions
    // as swept in ClearAllMarkBitmaps(). This is logical list reclassification,
    // not a real sweep pass.
    void ReclassifyUnsweptToSwept();

    // Re-enable lazy sweep after MarkSetup bitmap clearing is complete.
    void ReenableLazySweep();

private:
    struct SizeClassBucket {
        std::mutex lock;              // protects fullSwept, partialUnswept, fullUnswept only
        LockFreeRegionStack partialSwept;  // O1: lock-free CAS stack for P1 hot path
        MSRegionList fullSwept;
        MSRegionList partialUnswept;
        MSRegionList fullUnswept;
    };

    void PushEmptySweptRegion(MSRegionInfo* region);
    MSRegionInfo* TakeEmptySweptRegion(uint8_t cls);
    void ForceFinishUnsweptList(SizeClassBucket& bucket, MSRegionList& list, uint8_t cls, uint32_t sg,
                                size_t& sweptRegions, size_t& emptiedRegions,
                                size_t& retainedRegions, size_t& freedBytes);
    void ForceFinishAllUnswept();
    bool SweepUnsweptList(SizeClassBucket& bucket, MSRegionList& list, uint32_t sg,
                         size_t& sweptInSlice, size_t maxRegions, uint64_t deadlineUs,
                         size_t* emptiedInSlice = nullptr);
    std::array<uint8_t, kNumSizeClasses> BuildBackgroundSweepClassOrder(uint8_t preferredCls) const;

    static std::atomic<uint32_t> globalSweepGen_;
    std::atomic<bool> sweepDone_{true};
    std::atomic<bool> bgSweepRunning_{false};
    std::atomic<bool> stopSweepRequested_{false};    // Signal bg sweep to stop early
    std::atomic<bool> skipLazySweep_{false};          // Disable lazy sweep in GetRegion P2/P3
    std::atomic<size_t> activeLazySweepCount_{0};     // In-flight lazy sweep counter
    MSSweepStats sweepStats_;

    SizeClassBucket buckets_[kNumSizeClasses];
    MSRegionManager*  msRegionManager_;

    // Large region tracking (Tier 1: in-heap large objects, MS_REGION_LARGE)
    std::mutex largeLock_;
    MSRegionList largeRegionList_;

    // Large object hot cache: recently-freed large regions whose physical pages
    // are still resident. Allows reuse without page-fault overhead on next alloc.
    // Bucketed by regionCount (key = regionBytes / MS_REGION_SIZE) for O(1)
    // size-matched lookup. No size or total-bytes cap: the scavenger is responsible
    // for draining the cache when RSS exceeds the threshold.
    // Flushed at the start of each SweepLargeRegions call.
    struct LargeHotEntry {
        uintptr_t regionStart;
        size_t    regionBytes;
    };
    size_t GetMaxLargeHotCacheBytes() const;
    size_t GetMaxEmptySweptBytes() const;
    size_t GetMaxEmptySweptRegions() const;
    std::mutex largeHotCacheLock_;
    std::unordered_map<size_t, std::vector<LargeHotEntry>> largeHotCache_; // key = regionCount

    // Global cache of completely-empty small/medium regions.
    std::mutex emptySweptLock_;
    MSRegionList emptySweptCache_;

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
    BackgroundFeedStats BackgroundFillAllocCaches(uint8_t preferredCls = kNoPreferredFeedClass);

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
#endif // MRT_MS_CENTRAL_REGION_POOL_H
