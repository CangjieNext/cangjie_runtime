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
#include <map>
#include <algorithm>
#include <thread>
#include <functional>

#include "Heap/Allocator/Allocator.h"
#include "Base/Log.h"
#include "Heap/MAllocator/MSMarkedStats.h"
#include "Heap/MAllocator/MSParallelSweepPolicy.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSShardRouting.h"
#include "Heap/MAllocator/MSSizeClass.h"
#include "Heap/MAllocator/MSSweepDebtPolicy.h"

namespace MapleRuntime {

class MSRegionManager;
class AllocBufferMS;
class BaseObject;

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
        CHECK_DETAIL(region != nullptr, "MSRegionList::Prepend with null region");
        CHECK_DETAIL(region->nextRegionIdx == MSRegionInfo::INVALID_IDX &&
                         region->prevRegionIdx == MSRegionInfo::INVALID_IDX,
                     "[MS-GC] MSRegionList::Prepend: region already linked, region=%p next=%d prev=%d",
                     static_cast<void*>(region),
                     region->nextRegionIdx,
                     region->prevRegionIdx);
        int32_t idx = static_cast<int32_t>(MSRegionInfoArray::GetRegionIdx(region));
        region->prevRegionIdx = MSRegionInfo::INVALID_IDX;
        region->nextRegionIdx = headIdx_;
        if (headIdx_ != MSRegionInfo::INVALID_IDX) {
            MSRegionInfoArray::GetRegionInfoByIdx(headIdx_)->prevRegionIdx = idx;
        }
        headIdx_ = idx;
        count_.fetch_add(1, std::memory_order_relaxed);
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
        count_.fetch_sub(1, std::memory_order_relaxed);
        return head;
    }

    MSRegionInfo* PeekHead() const
    {
        if (headIdx_ == MSRegionInfo::INVALID_IDX) {
            return nullptr;
        }
        return MSRegionInfoArray::GetRegionInfoByIdx(headIdx_);
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
        count_.fetch_sub(1, std::memory_order_relaxed);
    }

    bool IsEmpty() const { return headIdx_ == MSRegionInfo::INVALID_IDX; }
    size_t Count() const { return count_.load(std::memory_order_relaxed); }

    void Swap(MSRegionList& other)
    {
        std::swap(headIdx_, other.headIdx_);
        size_t thisCount = count_.load(std::memory_order_relaxed);
        size_t otherCount = other.count_.load(std::memory_order_relaxed);
        count_.store(otherCount, std::memory_order_relaxed);
        other.count_.store(thisCount, std::memory_order_relaxed);
    }

    // O(1) accept a single-linked chain (nextRegionIdx only, prev NOT set).
    // List MUST be empty. Safe when consumers only use TakeHead (which fixes prev lazily).
    void AcceptSingleLinkedChain(int32_t headIdx, size_t count)
    {
        headIdx_ = headIdx;
        count_.store(count, std::memory_order_relaxed);
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
    std::atomic<size_t> count_{0};
};

// ============================================================================
// LockFreeRegionStack: CAS-based lock-free stack for partialSwept (O1)
// ============================================================================
// Single-linked stack using MSRegionInfo::nextRegionIdx.
//
// ABA SAFETY: the earlier "exclusive ownership after Pop" argument was WRONG —
// a region can be popped, filled, emptied and re-pushed by other threads while
// one thread's Pop CAS on the same head is still in flight. With a bare index
// head the value can return to its original (ABA), the stale CAS wrongly
// succeeds, and the same region is handed to two owners (→ two objects share a
// slot → use-after-free). To prevent this the head is a 64-bit value packing
// { tag:32 | regionIndex:32 }; every Push/Pop bumps the tag.
//
class LockFreeRegionStack {
public:
    LockFreeRegionStack() = default;

    void Push(MSRegionInfo* region)
    {
        CHECK_DETAIL(region != nullptr, "LockFreeRegionStack::Push with null region");
        CHECK_DETAIL(region->nextRegionIdx == MSRegionInfo::INVALID_IDX &&
                         region->prevRegionIdx == MSRegionInfo::INVALID_IDX,
                     "[MS-GC] LockFreeRegionStack::Push: region already linked, region=%p next=%d prev=%d",
                     static_cast<void*>(region),
                     region->nextRegionIdx,
                     region->prevRegionIdx);
        int32_t idx = static_cast<int32_t>(MSRegionInfoArray::GetRegionIdx(region));
        uint64_t oldVal = taggedHead_.load(std::memory_order_relaxed);
        uint64_t newVal;
        do {
            region->nextRegionIdx = IdxOf(oldVal);
            newVal = Pack(idx, TagOf(oldVal) + 1);
        } while (!taggedHead_.compare_exchange_weak(oldVal, newVal,
                     std::memory_order_release, std::memory_order_relaxed));
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    MSRegionInfo* Pop()
    {
        uint64_t oldVal = taggedHead_.load(std::memory_order_acquire);
        MSRegionInfo* headRegion;
        uint64_t newVal;
        do {
            int32_t oldHead = IdxOf(oldVal);
            if (oldHead == MSRegionInfo::INVALID_IDX) {
                return nullptr;
            }
            headRegion = MSRegionInfoArray::GetRegionInfoByIdx(oldHead);
            // Bump the tag on every successful Pop so that a region which is
            // popped, reused and re-pushed while this CAS is in flight cannot
            // make the head value compare equal again (ABA). Reading
            // nextRegionIdx of a concurrently-recycled region is harmless: the
            // tag will have changed and the CAS below simply retries.
            newVal = Pack(headRegion->nextRegionIdx, TagOf(oldVal) + 1);
        } while (!taggedHead_.compare_exchange_weak(oldVal, newVal,
                     std::memory_order_release, std::memory_order_relaxed));
        headRegion->nextRegionIdx = MSRegionInfo::INVALID_IDX;
        headRegion->prevRegionIdx = MSRegionInfo::INVALID_IDX;
        count_.fetch_sub(1, std::memory_order_relaxed);
        return headRegion;
    }

    bool IsEmpty() const
    {
        return IdxOf(taggedHead_.load(std::memory_order_acquire)) == MSRegionInfo::INVALID_IDX;
    }
    size_t Count() const { return count_.load(std::memory_order_relaxed); }

    // O(1) transfer entire chain to MSRegionList. Must be called during STW.
    // Destination MUST be empty. Prev pointers are NOT fixed — safe when
    // consumers only use TakeHead (which fixes prev lazily).
    void StealChainTo(MSRegionList& dest)
    {
        uint64_t v = taggedHead_.load(std::memory_order_relaxed);
        size_t cnt = count_.load(std::memory_order_relaxed);
        taggedHead_.store(Pack(MSRegionInfo::INVALID_IDX, TagOf(v) + 1), std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
        dest.AcceptSingleLinkedChain(IdxOf(v), cnt);
    }

    // Unsafe iteration for GC (STW only). Visitor must NOT modify the stack.
    template<typename Visitor>
    void ForEachUnsafe(Visitor&& visitor) const
    {
        int32_t idx = IdxOf(taggedHead_.load(std::memory_order_acquire));
        while (idx != MSRegionInfo::INVALID_IDX) {
            MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoByIdx(idx);
            int32_t next = info->nextRegionIdx;
            visitor(info);
            idx = next;
        }
    }

private:
    // taggedHead_ packs { tag:high32 | regionIndex:low32 }. The tag is bumped on
    // every Push/Pop so a recycled region index never makes the head compare
    // equal to a stale snapshot — this is what makes the stack ABA-safe.
    static int32_t IdxOf(uint64_t v) { return static_cast<int32_t>(static_cast<uint32_t>(v)); }
    static uint32_t TagOf(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
    static uint64_t Pack(int32_t idx, uint32_t tag)
    {
        return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(idx));
    }

    std::atomic<uint64_t> taggedHead_{Pack(MSRegionInfo::INVALID_IDX, 0)};
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
    friend class AllocBufferMS;

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

    // Fast path: take an already-swept partial region for the given size class.
    MSRegionInfo* TakePartialSweptRegion(uint8_t cls);

    // Slow path: lazily sweep partial/full unswept lists and return a usable region.
    MSRegionInfo* TakeUnsweptRegion(uint8_t cls);

    // Get a fresh mutator Region from RegionManager after central lists miss.
    // This path skips releasedTree and goes directly to bump/grow.
    MSRegionInfo* GetFreshRegion(uint8_t cls, uint16_t shardHint = kMSInvalidShardId);
    uint16_t ResolveHomeShardId(uint16_t shardHint = kMSInvalidShardId) const;

    // ===== Thread-local Region return =====

    void ReturnPartialRegion(MSRegionInfo* region, uint8_t cls);
    void ReturnFullRegion(MSRegionInfo* region, uint8_t cls);
    void ReturnEmptyRegion(MSRegionInfo* region);

    // ===== GC operations =====

    // Prepare the next sweep epoch.
    // 1) Flip central swept lists into unswept supply for the coming cycle.
    // 2) Normalize any residual old-generation unswept/large regions to the
    //    current generation before advancing globalSweepGen_.
    // 3) globalSweepGen_ += 2 so subsequent allocations carry the new epoch.
    void PrepareForSweep();

    // Background sweep: iterate all size classes, sweep one slice of unswept Regions.
    // Thread-safe (CAS ownership). Called by gc-helper thread.
    void BackgroundSweep(uint8_t preferredCls, uint64_t deadlineUs);
    void ParallelSweep(uint8_t preferredCls, uint64_t deadlineUs);
    // Repay one P's local Sweep debt. Non-assist Sweep work is claimed once from
    // the central credit pool; a remaining full batch is swept directly once.
    MSSweepAssistResult MSSweepAssist(AllocBufferMS& owner, size_t debtRegions,
                                      uint8_t preferredCls);
    uint64_t GetSweepAssistRatioScaled() const
    {
        uint64_t base = sweepRegionsPerAllocScaled_.load(std::memory_order_acquire);
        return ScaleMSSweepAssistRatioScaled(base, GetSweepPressureMode());
    }

    // Recompute LOW/MEDIUM/HIGH from unswept bytes, swept-retained bytes and the
    // heap threshold. A Notify-forced HIGH remains latched until Sweep completes.
    SweepPressureMode RefreshSweepDebtSnapshot(size_t thresholdBytes, size_t sweptBytes);
    void ForceHighSweepPressure();
    SweepPressureMode GetSweepPressureMode() const
    {
        return static_cast<SweepPressureMode>(sweepPressureMode_.load(std::memory_order_relaxed));
    }
    bool IsSweepPressureForcedHigh() const
    {
        return forceHighSweepPressure_.load(std::memory_order_acquire);
    }

    bool HasPendingSweepWork() const
    {
        return !stopSweepRequested_.load(std::memory_order_acquire) &&
               GetTotalUnsweptCount() > 0;
    }

    // Reclaim up to maxRegions empty regions from the empty-region caches into released
    // state. Passing a very large maxRegions value releases all currently cached empty regions.
    size_t ReclaimEmptyRegion(size_t maxRegions);
    bool HasIdleSegmentCachedRegions(size_t startRegionIdx, size_t endRegionIdx) const;
    size_t ReclaimIdleSegmentCaches(size_t startRegionIdx, size_t endRegionIdx);

    // Check whether previous-cycle sweep work is globally quiescent.
    bool IsSweepDone() const
    {
        return !forceFinishSweepRunning_.load(std::memory_order_acquire) &&
            IsMSSweepQuiescent(
            sweepDone_.load(std::memory_order_acquire),
            bgSweepRunning_.load(std::memory_order_acquire),
            activeLazySweepCount_.load(std::memory_order_acquire),
            activeAssistSweepCount_.load(std::memory_order_acquire),
            activeHelperSweepCount_.load(std::memory_order_acquire));
    }

    // Visit all Regions across all size classes (for GC root enumeration).
    template<typename Visitor>
    void VisitAllRegions(Visitor&& visitor);

    // ===== Monitoring =====
    size_t GetTotalUnsweptCount() const;
    size_t GetTotalUnsweptCountReference() const;
    size_t GetTotalUnsweptBytes() const;
    size_t GetLargeRegionCount() const;
    size_t CountTLActiveRegions() const;
    size_t GetAllocBufferCount() const;
    size_t GetAllAllocBufferActiveRegionCount() const;
    BucketPressure GetBucketPressure(uint8_t cls) const;
    void MaybeNotifySweep(uint8_t cls) const;
    // Swept-retained region count: partialSwept + fullSwept buckets + large region spans.
    // Excludes unswept regions; used by MaybeNotifyMark to gate the next GC on the
    // post-sweep live footprint rather than the transient active-region total.
    size_t GetSweptMSRegionCount() const;
    size_t GetActiveMSRegionCount() const;
    size_t GetSweptMSRegionCountReference() const;
    size_t GetActiveMSRegionCountReference() const;
    size_t GetEmptySweptRegionCount() const;
    size_t GetLargeHotCacheRegionCount() const;

    // Dump CentralRegionPool 4-list statistics (block ⑤ in DFX design)
    void DumpPoolStats() const;

    static uint32_t GetGlobalSweepGen()
    {
        return globalSweepGen_.load(std::memory_order_acquire);
    }

    // Compute marked object count and true live bytes in one bitmap walk.
    // Must be called after marking/large eager reclaim complete and before
    // PostMarkTermination publishes small-Region Sweep work.
    MSMarkedStats ComputeMarkedStats() const;

    // Diagnostic: visit every currently-marked object (small/medium + large)
    // by walking markBitmaps. Must be called in the same window as
    // ComputeMarkedStats (marking complete, no sweep started). Used by the
    // mark-completeness verifier to detect live objects that were missed.
    void VisitMarkedObjects(const std::function<void(BaseObject*)>& func) const;

    // ===== Large region management (Tier 1: in-heap large objects) =====

    // Register a large MSRegionInfo (MS_REGION_LARGE) for GC tracking.
    void RegisterLargeRegion(MSRegionInfo* region);

    // Eager sweep: reclaim unmarked large regions, return freed bytes.
    // Called after MarkTerminate / in PostMarkTermination.
    void SweepLargeRegions();

    // Large object hot cache: cache a freshly-freed large region so its
    // physical pages remain resident for fast reuse on the next allocation.
    // Returns false only when regionBytes is invalid.
    bool TryPutLargeHotCache(uintptr_t regionStart, size_t regionBytes);

    // Pop a cached entry matching regionBytes from the hot cache.
    // Returns true and sets regionStart if found; false if miss.
    bool TryTakeLargeHotCache(size_t regionBytes, uintptr_t& regionStart);

    // Reclaim large-hot-cache entries back to MSRegionManager (releasedTree),
    // largest entries first, up to maxBytes (SIZE_MAX = drain all).
    // This is the single low-level release primitive for the large hot cache;
    // flush policy lives in RegionSpaceMS. Returns reclaimed bytes.
    size_t ReclaimLargeHotCache(size_t maxBytes);

    size_t GetLargeHotCacheBytes() const
    {
        return largeHotCacheBytes_.load(std::memory_order_acquire);
    }

    // ===== Pre-STW bitmap clearing (MarkSetup fast path) =====

    // Cooperatively drain the old unswept backlog while all Sweep sources are
    // still open, then close background/lazy/assist/helper admission and wait
    // for their in-flight work to quiesce. A final serial drain closes anything
    // left during the handoff; only then may sweepDone be published. The gates
    // remain closed through Mark and must be reopened by ReenableLazySweep().
    // Must finish before clearing markBitmaps to avoid racing SweepRegion reads.
    void StopAndWaitForAllSweep();

    // Clear markBitmaps for all active small/medium regions in MSRegionInfoArray.
    // Large regions clear their mark flag here as well.
    // Must be called after StopAndWaitForAllSweep().
    void ClearAllMarkBitmaps();

    // Drain any residual unswept lists into swept lists before the next epoch
    // flip. This is logical list reclassification, not a real sweep pass.
    void ReclassifyUnsweptToSwept();

    // Re-enable lazy/assist sweep after MarkSetup bitmap clearing is complete.
    void ReenableLazySweep();

private:
    class SizeClassBucket {
    public:
        enum class UnsweptListKind : uint8_t {
            PARTIAL,
            FULL,
        };

        void BindAggregateCounters(std::atomic<size_t>* activeRegions,
                                   std::atomic<size_t>* sweptRegions,
                                   std::atomic<size_t>* unsweptRegions)
        {
            activeRegions_ = activeRegions;
            sweptRegions_ = sweptRegions;
            unsweptRegions_ = unsweptRegions;
        }

        MSRegionInfo* TakePartialSwept()
        {
            MSRegionInfo* region = partialSwept_.Pop();
            if (region != nullptr) {
                LeaveCentralList(true);
            }
            return region;
        }

        MSRegionInfo* ClaimUnsweptHeadForSweep(UnsweptListKind kind, uint32_t sg)
        {
            std::lock_guard<std::mutex> lg(lock_);
            MSRegionList& list = GetUnsweptList(kind);
            for (;;) {
                MSRegionInfo* head = list.PeekHead();
                if (head == nullptr) {
                    return nullptr;
                }

                uint32_t headSweepGen = head->GetSweepGen();
                if (headSweepGen == sg) {
                    (void)list.TakeHead();
                    LeaveCentralList(false);
                    continue;
                }

                if (UNLIKELY(headSweepGen == sg - 1)) {
                    LOG(RTLOG_FATAL,
                        "[MS-GC] ClaimUnsweptHeadForSweep: observed in-list sweeping region, "
                        "cls=%u region=%p sweepGen=%u type=%u",
                        static_cast<unsigned>(head->sizeClass), static_cast<void*>(head),
                        static_cast<unsigned>(headSweepGen),
                        static_cast<unsigned>(head->regionType));
                }

                if (UNLIKELY(headSweepGen != sg - 2)) {
                    LOG(RTLOG_FATAL,
                        "[MS-GC] ClaimUnsweptHeadForSweep: unexpected in-list sweepGen, "
                        "cls=%u region=%p sweepGen=%u expectedUnswept=%u type=%u",
                        static_cast<unsigned>(head->sizeClass), static_cast<void*>(head),
                        static_cast<unsigned>(headSweepGen),
                        static_cast<unsigned>(sg - 2),
                        static_cast<unsigned>(head->regionType));
                }

                if (!head->CasSweepGen(sg - 2, sg - 1)) {
                    continue;
                }

                MSRegionInfo* claimed = list.TakeHead();
                if (UNLIKELY(claimed != head)) {
                    LOG(RTLOG_FATAL,
                        "[MS-GC] ClaimUnsweptHeadForSweep: head changed under bucket lock, "
                        "expected=%p actual=%p cls=%u kind=%u",
                        static_cast<void*>(head), static_cast<void*>(claimed),
                        static_cast<unsigned>(head->sizeClass),
                        static_cast<unsigned>(kind));
                }
                MSRegionInfoArray::TransitionState(
                    claimed,
                    kind == UnsweptListKind::PARTIAL
                        ? MSRegionLocation::CENTRAL_PARTIAL
                        : MSRegionLocation::CENTRAL_FULL,
                    MSRegionLocation::SWEEP_CLAIMED,
                    MSRegionInfoArray::ANY_OWNER,
                    0,
                    "ClaimUnsweptHeadForSweep");
                LeaveCentralList(false);
                return claimed;
            }
        }

        void PushPartialSwept(MSRegionInfo* region)
        {
            // Publish the aggregate before the lock-free list node. A concurrent
            // Pop may observe the node immediately and must never subtract from
            // an aggregate that has not recorded this Region yet.
            EnterCentralList(true);
            partialSwept_.Push(region);
        }

        void PushPartialUnswept(MSRegionInfo* region)
        {
            std::lock_guard<std::mutex> lg(lock_);
            EnterCentralList(false);
            partialUnswept_.Prepend(region);
        }

        void PushFullSwept(MSRegionInfo* region)
        {
            std::lock_guard<std::mutex> lg(lock_);
            EnterCentralList(true);
            fullSwept_.Prepend(region);
        }

        void PushFullUnswept(MSRegionInfo* region)
        {
            std::lock_guard<std::mutex> lg(lock_);
            EnterCentralList(false);
            fullUnswept_.Prepend(region);
        }

        void FlipSweptToUnswept()
        {
            std::lock_guard<std::mutex> lg(lock_);
            size_t sweptBefore = CountPartialSwept() + CountFullSwept();
            partialSwept_.StealChainTo(partialUnswept_);
            fullSwept_.Swap(fullUnswept_);
            size_t sweptAfter = CountPartialSwept() + CountFullSwept();
            AdjustSweptAggregate(sweptBefore, sweptAfter);
        }

        void ReclassifyUnsweptToSwept(size_t& partialMoved, size_t& fullMoved)
        {
            std::lock_guard<std::mutex> lg(lock_);
            size_t sweptBefore = CountPartialSwept() + CountFullSwept();
            MSRegionInfo* region = nullptr;
            while ((region = partialUnswept_.TakeHead()) != nullptr) {
                partialSwept_.Push(region);
                partialMoved++;
            }
            while ((region = fullUnswept_.TakeHead()) != nullptr) {
                fullSwept_.Prepend(region);
                fullMoved++;
            }
            size_t sweptAfter = CountPartialSwept() + CountFullSwept();
            AdjustSweptAggregate(sweptBefore, sweptAfter);
        }

        size_t CountPartialSwept() const
        {
            return partialSwept_.Count();
        }

        size_t CountFullSwept() const
        {
            return fullSwept_.Count();
        }

        size_t CountPartialUnswept() const
        {
            return partialUnswept_.Count();
        }

        size_t CountFullUnswept() const
        {
            return fullUnswept_.Count();
        }

        BucketPressure GetPressure() const
        {
            BucketPressure pressure;
            pressure.partialSwept = CountPartialSwept();
            pressure.fullSwept = CountFullSwept();
            pressure.partialUnswept = CountPartialUnswept();
            pressure.fullUnswept = CountFullUnswept();
            return pressure;
        }

        template<typename Visitor>
        void VisitPartialSweptUnsafe(Visitor&& visitor) const
        {
            partialSwept_.ForEachUnsafe(visitor);
        }

        template<typename Visitor>
        void VisitAllLocked(Visitor&& visitor) const
        {
            std::lock_guard<std::mutex> lg(lock_);
            fullSwept_.ForEach(visitor);
            partialUnswept_.ForEach(visitor);
            fullUnswept_.ForEach(visitor);
        }

        template<typename FullSweptVisitor, typename PartialUnsweptVisitor, typename FullUnsweptVisitor>
        void VisitLockedLists(FullSweptVisitor&& fullSweptVisitor,
                              PartialUnsweptVisitor&& partialUnsweptVisitor,
                              FullUnsweptVisitor&& fullUnsweptVisitor) const
        {
            std::lock_guard<std::mutex> lg(lock_);
            fullSwept_.ForEach(fullSweptVisitor);
            partialUnswept_.ForEach(partialUnsweptVisitor);
            fullUnswept_.ForEach(fullUnsweptVisitor);
        }

    private:
        MSRegionList& GetUnsweptList(UnsweptListKind kind)
        {
            return kind == UnsweptListKind::PARTIAL ? partialUnswept_ : fullUnswept_;
        }

        static void AddAggregate(std::atomic<size_t>* counter, size_t count,
                                 std::memory_order order = std::memory_order_relaxed)
        {
            if (counter != nullptr && count != 0) {
                counter->fetch_add(count, order);
            }
        }

        static void SubAggregate(std::atomic<size_t>* counter, size_t count, const char* name,
                                 std::memory_order order = std::memory_order_relaxed)
        {
            if (counter == nullptr || count == 0) {
                return;
            }
            size_t old = counter->fetch_sub(count, order);
            CHECK_DETAIL(old >= count,
                         "[MS-GC] %s aggregate underflow: old=%zu count=%zu",
                         name, old, count);
        }

        void EnterCentralList(bool swept)
        {
            AddAggregate(activeRegions_, 1);
            if (swept) {
                AddAggregate(sweptRegions_, 1);
            } else {
                // The aggregate is visible before the unswept list node is
                // published, so completion cannot miss newly queued work.
                AddAggregate(unsweptRegions_, 1, std::memory_order_acq_rel);
            }
        }

        void LeaveCentralList(bool swept)
        {
            SubAggregate(activeRegions_, 1, "centralSmallActiveRegions");
            if (swept) {
                SubAggregate(sweptRegions_, 1, "centralSmallSweptRegions");
            } else {
                // ClaimUnsweptHeadForSweep calls this only after uniquely
                // removing the list node, including stale current-gen heads.
                SubAggregate(unsweptRegions_, 1, "centralSmallUnsweptRegions",
                             std::memory_order_acq_rel);
            }
        }

        void AdjustSweptAggregate(size_t before, size_t after)
        {
            if (after > before) {
                AddAggregate(sweptRegions_, after - before);
            } else if (before > after) {
                SubAggregate(sweptRegions_, before - after, "centralSmallSweptRegions");
            }
        }

        std::atomic<size_t>* activeRegions_ = nullptr;
        std::atomic<size_t>* sweptRegions_ = nullptr;
        std::atomic<size_t>* unsweptRegions_ = nullptr;
        mutable std::mutex lock_;              // protects fullSwept_, partialUnswept_, fullUnswept_
        LockFreeRegionStack partialSwept_;     // O1: lock-free CAS stack for P1 hot path
        MSRegionList fullSwept_;
        MSRegionList partialUnswept_;
        MSRegionList fullUnswept_;
    };

    void PushEmptySweptRegion(MSRegionInfo* region);
    MSRegionInfo* TakeGlobalEmptyRegion();
    size_t DrainEmptySweptGlobal(size_t maxRegions, std::vector<MSRegionInfo*>& drained);
    size_t GetEmptySweptGlobalCount() const
    {
        return emptySweptCache_.Count();
    }
    void ForceFinishUnsweptList(SizeClassBucket& bucket, SizeClassBucket::UnsweptListKind listKind,
                                uint32_t sg, size_t& sweptRegions, size_t& emptiedRegions,
                                size_t& retainedRegions, size_t& freedBytes);
    void ForceFinishAllUnswept();
    bool TryPublishSweepDone();
    size_t AssistSweep(size_t targetRegions, uint8_t preferredCls, AllocBufferMS* preferredOwner);
    bool SweepOneUnsweptRegionForAssist(SizeClassBucket& bucket, SizeClassBucket::UnsweptListKind listKind,
                                        uint32_t sg, AllocBufferMS* preferredOwner,
                                        size_t* emptiedInSlice = nullptr);
    bool SweepUnsweptList(SizeClassBucket& bucket, SizeClassBucket::UnsweptListKind listKind, uint32_t sg,
                         size_t& sweptInSlice, size_t maxRegions, uint64_t deadlineUs,
                         size_t* emptiedInSlice = nullptr);
    std::array<uint8_t, kNumSizeClasses> BuildBackgroundSweepClassOrder(uint8_t preferredCls) const;
    void IncrementEmptySweptCount(size_t count = 1)
    {
        emptySweptCount_.fetch_add(count, std::memory_order_release);
    }
    void DecrementEmptySweptCount(size_t count = 1)
    {
        if (count == 0) {
            return;
        }
        size_t oldCount = emptySweptCount_.load(std::memory_order_acquire);
        for (;;) {
            size_t newCount = oldCount > count ? oldCount - count : 0;
            if (emptySweptCount_.compare_exchange_weak(oldCount, newCount,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
                return;
            }
        }
    }
    void RecordBackgroundSweep(size_t sweptRegions)
    {
        if (sweptRegions != 0) {
            sweepCredit_.Add(sweptRegions);
        }
    }
    void RecordLazySweep(size_t sweptRegions)
    {
        if (sweptRegions != 0) {
            sweepCredit_.Add(sweptRegions);
        }
    }
    void RecordHelperSweep(size_t sweptRegions)
    {
        if (sweptRegions != 0) {
            sweepCredit_.Add(sweptRegions);
        }
    }
    static std::atomic<uint32_t> globalSweepGen_;
    std::atomic<bool> sweepDone_{true};
    std::atomic<bool> bgSweepRunning_{false};
    std::atomic<bool> stopSweepRequested_{false};    // Signal bg sweep to stop early
    std::atomic<bool> skipLazySweep_{false};          // Disable lazy sweep in GetRegion P2/P3
    std::atomic<bool> skipAssistSweep_{false};        // Disable mutator assist sweep before STW
    std::atomic<bool> skipHelperSweep_{false};        // Disable GC-helper pool sweep before STW
    std::atomic<size_t> activeLazySweepCount_{0};     // In-flight lazy sweep counter
    std::atomic<size_t> activeAssistSweepCount_{0};   // In-flight mutator assist sweep counter
    std::atomic<size_t> activeHelperSweepCount_{0};   // In-flight parallel helper sweep counter
    std::atomic<bool> forceFinishSweepRunning_{false}; // GC main owns ForceFinishAllUnswept
    // ===== P-local proportional Sweep pacing =====
    // PrepareForSweep publishes one fixed-point ratio for the generation.
    // AllocBufferMS owns allocation progress and debt; central non-assist work is
    // converted into CAS-consumed credit so a swept Region pays at most one P.
    std::atomic<uint64_t> sweepRegionsPerAllocScaled_ {0};
    MSSweepCreditPool sweepCredit_;

    // Unified Sweep pressure snapshot and Notify-forced epoch latch.
    std::atomic<int> sweepPressureMode_{static_cast<int>(SweepPressureMode::LOW)};
    std::atomic<bool> forceHighSweepPressure_{false};

    std::atomic<size_t> centralSmallActiveRegions_ {0};
    std::atomic<size_t> centralSmallSweptRegions_ {0};
    std::atomic<size_t> centralSmallUnsweptRegions_ {0};
    SizeClassBucket buckets_[kNumSizeClasses];
    MSRegionManager*  msRegionManager_;

    // Large region tracking (Tier 1: in-heap large objects, MS_REGION_LARGE)
    std::mutex largeLock_;
    MSRegionList largeRegionList_;
    std::atomic<size_t> largeLiveSpanCount_{0};
    void AddLargeLiveRegion(MSRegionInfo* region);
    void CollectDeadLargeRegions(std::vector<MSRegionInfo*>& deadRegions);
    template<typename Visitor>
    void VisitLargeRegionsLocked(Visitor&& visitor) const
    {
        std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(largeLock_));
        largeRegionList_.ForEach(visitor);
    }

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
    std::mutex largeHotCacheLock_;
    std::map<size_t, std::vector<LargeHotEntry>> largeHotCache_; // key = regionCount
    std::atomic<size_t> largeHotCacheRegionCount_{0};
    std::atomic<size_t> largeHotCacheBytes_{0};

    // Global cache of classless empty regions. AllocBufferMS owns any local empty
    // cache and may batch-refill from this global queue. emptySweptCount_ tracks
    // the total number of empty-cache regions across both global and local caches.
    std::mutex emptySweptLock_;
    MSRegionList emptySweptCache_;
    std::atomic<size_t> emptySweptCount_{0};

    // ===== Per-thread AllocBufferMS registry =====
    // Every thread lazily creates an AllocBufferMS and registers it here.
public:
    void RegisterAllocBufferMS(AllocBufferMS* buffer);
    void RemoveAllocBufferMS(AllocBufferMS* buffer);

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
        bucket.VisitPartialSweptUnsafe(visitor);
        bucket.VisitAllLocked(visitor);
    }
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_CENTRAL_REGION_POOL_H
