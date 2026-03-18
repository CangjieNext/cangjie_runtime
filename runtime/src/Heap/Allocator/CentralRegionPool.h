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

    void Reset() {
        bgSweptRegions.store(0, std::memory_order_relaxed);
        lazySweptRegions.store(0, std::memory_order_relaxed);
        forceSweptRegions.store(0, std::memory_order_relaxed);
        sweptFreedBytes.store(0, std::memory_order_relaxed);
        releasedRegions.store(0, std::memory_order_relaxed);
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

    // Drain all elements into an MSRegionList (under external synchronization, e.g. STW).
    // Used by PrepareForSweep to swap partialSwept into partialUnswept.
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
    // Corresponds to Go's finishsweep_m().
    void ForceFinishSweep();

    // Generation flip: globalSweepGen_ += 2.
    // All swept Regions logically become unswept.
    void PrepareForSweep();

    // Background sweep: iterate all size classes, sweep unswept Regions.
    // Thread-safe (CAS ownership). Called by gc-helper thread.
    void BackgroundSweep();

    // Check if background/forced sweep has completed.
    bool IsSweepDone() const { return sweepDone_.load(std::memory_order_acquire); }

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

    // Clear isMarked on all tracked large regions (called in MarkSetup).
    void ClearLargeMarks();

    // Eager sweep: reclaim unmarked large regions, return freed bytes.
    // Called after MarkTerminate / in PostMarkTermination.
    size_t SweepLargeRegions();

    // Wait for all sweep (background + lazy) to finish.
    // Called in MarkSetup before ForceFinishSweep to prevent data race:
    // bg sweep's SweepRegion (memset markBitmap 0) vs next cycle's ConcurrentMark.
    void WaitForSweepDone()
    {
        while (!sweepDone_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

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
    MSSweepStats sweepStats_;

    // Total bytes of MS regions currently in use (GetFreshRegion +, ReleaseRegion -)
    // Used by AllocatedBytes() for GC trigger threshold check.
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
