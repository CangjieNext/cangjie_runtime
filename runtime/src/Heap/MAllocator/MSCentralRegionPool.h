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
#include <utility>

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

// GC-private small-Region chain. Only the GC main thread mutates head/tail/count
// while the world is stopped or after Mark has quiesced. Nodes reuse
// MSRegionInfo::centralNextRegionIdx; the chain itself needs no lock or tag.
struct RegionChain {
    int32_t headIdx = MSRegionInfo::INVALID_IDX;
    int32_t tailIdx = MSRegionInfo::INVALID_IDX;
    size_t count = 0;

    bool IsEmpty() const
    {
        return headIdx == MSRegionInfo::INVALID_IDX;
    }

    void Reset()
    {
        headIdx = MSRegionInfo::INVALID_IDX;
        tailIdx = MSRegionInfo::INVALID_IDX;
        count = 0;
    }

    void Append(RegionChain&& other)
    {
        if (other.IsEmpty()) {
            return;
        }
        if (IsEmpty()) {
            *this = other;
        } else {
            MSRegionInfoArray::GetRegionInfoByIdx(tailIdx)->SetCentralNextRegionIdx(
                static_cast<uint32_t>(other.headIdx));
            tailIdx = other.tailIdx;
            count += other.count;
        }
        other.Reset();
    }

    template<typename Visitor>
    void ForEach(Visitor&& visitor) const
    {
        int32_t idx = headIdx;
        while (idx != MSRegionInfo::INVALID_IDX) {
            MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoByIdx(idx);
            int32_t next = static_cast<int32_t>(info->GetCentralNextRegionIdx());
            visitor(info);
            idx = next;
        }
    }
};

// ============================================================================
// MSRegionList: lightweight intrusive singly-linked list over MSRegionInfo
// ============================================================================
// Uses MSRegionInfo::centralNextRegionIdx for linking.
// Caller must hold the bucket lock; no internal locking.
//
class MSRegionList {
public:
    MSRegionList() = default;

    void Prepend(MSRegionInfo* region)
    {
        CHECK_DETAIL(region != nullptr, "MSRegionList::Prepend with null region");
        CHECK_DETAIL(region->GetCentralNextRegionIdx() == MSRegionInfo::INVALID_REGION_IDX,
                     "[MS-GC] MSRegionList::Prepend: region already linked, region=%p next=%u",
                     static_cast<void*>(region),
                     region->GetCentralNextRegionIdx());
        int32_t idx = static_cast<int32_t>(MSRegionInfoArray::GetRegionIdx(region));
        region->SetCentralNextRegionIdx(static_cast<uint32_t>(headIdx_));
        headIdx_ = idx;
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    MSRegionInfo* TakeHead()
    {
        if (headIdx_ == MSRegionInfo::INVALID_IDX) {
            return nullptr;
        }
        MSRegionInfo* head = MSRegionInfoArray::GetRegionInfoByIdx(headIdx_);
        int32_t nextIdx = static_cast<int32_t>(head->GetCentralNextRegionIdx());
        headIdx_ = nextIdx;
        head->SetCentralNextRegionIdx(MSRegionInfo::INVALID_REGION_IDX);
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
        int32_t targetIdx = static_cast<int32_t>(MSRegionInfoArray::GetRegionIdx(region));
        MSRegionInfo* previous = nullptr;
        int32_t current = headIdx_;
        while (current != MSRegionInfo::INVALID_IDX && current != targetIdx) {
            previous = MSRegionInfoArray::GetRegionInfoByIdx(current);
            current = static_cast<int32_t>(previous->GetCentralNextRegionIdx());
        }
        CHECK_DETAIL(current == targetIdx,
                     "[MS-GC] MSRegionList::Remove: region is not a member, idx=%d",
                     targetIdx);
        uint32_t next = region->GetCentralNextRegionIdx();
        if (previous == nullptr) {
            headIdx_ = static_cast<int32_t>(next);
        } else {
            previous->SetCentralNextRegionIdx(next);
        }
        region->SetCentralNextRegionIdx(MSRegionInfo::INVALID_REGION_IDX);
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

    // O(1) accept a single-linked centralNextRegionIdx chain.
    void AcceptSingleLinkedChain(int32_t headIdx, size_t count)
    {
        headIdx_ = headIdx;
        count_.store(count, std::memory_order_relaxed);
    }

    // O(1) prepend of a GC-private chain. Caller owns both containers.
    void PrependChain(RegionChain&& chain)
    {
        if (chain.IsEmpty()) {
            return;
        }
        MSRegionInfoArray::GetRegionInfoByIdx(chain.tailIdx)->SetCentralNextRegionIdx(
            static_cast<uint32_t>(headIdx_));
        headIdx_ = chain.headIdx;
        count_.fetch_add(chain.count, std::memory_order_relaxed);
        chain.Reset();
    }

    RegionChain StealChain()
    {
        RegionChain chain;
        chain.headIdx = headIdx_;
        chain.count = count_.exchange(0, std::memory_order_relaxed);
        headIdx_ = MSRegionInfo::INVALID_IDX;
        int32_t idx = chain.headIdx;
        while (idx != MSRegionInfo::INVALID_IDX) {
            chain.tailIdx = idx;
            MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoByIdx(idx);
            idx = static_cast<int32_t>(info->GetCentralNextRegionIdx());
        }
        CHECK_DETAIL((chain.count == 0) == chain.IsEmpty(),
                     "[MS-GC] MSRegionList::StealChain count/head mismatch");
        return chain;
    }

    // Iterate all elements, calling visitor(MSRegionInfo*) for each.
    // Visitor may NOT modify the list (use TakeHead loop for drain).
    template<typename Visitor>
    void ForEach(Visitor&& visitor) const
    {
        int32_t idx = headIdx_;
        while (idx != MSRegionInfo::INVALID_IDX) {
            MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoByIdx(idx);
            int32_t nextIdx = static_cast<int32_t>(info->GetCentralNextRegionIdx());
            visitor(info);
            idx = nextIdx;
        }
    }

private:
    int32_t headIdx_ = MSRegionInfo::INVALID_IDX;
    std::atomic<size_t> count_{0};
};

// ============================================================================
// LockFreeRegionStack: ABA-safe tagged stack shared by all central Region heads.
// ============================================================================
// Single-linked stack using MSRegionInfo::centralNextRegionIdx.
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
        CHECK_DETAIL(region->GetCentralNextRegionIdx() == MSRegionInfo::INVALID_REGION_IDX,
                     "[MS-GC] LockFreeRegionStack::Push: region already linked, region=%p next=%u",
                     static_cast<void*>(region),
                     region->GetCentralNextRegionIdx());
        int32_t idx = static_cast<int32_t>(MSRegionInfoArray::GetRegionIdx(region));
        uint64_t oldVal = taggedHead_.load(std::memory_order_relaxed);
        uint64_t newVal;
        do {
            region->SetCentralNextRegionIdx(static_cast<uint32_t>(IdxOf(oldVal)));
            newVal = Pack(idx, TagOf(oldVal) + 1);
        } while (!taggedHead_.compare_exchange_weak(oldVal, newVal,
                     std::memory_order_acq_rel, std::memory_order_acquire));
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
            // centralNextRegionIdx of a concurrently-recycled region is harmless: the
            // tag will have changed and the CAS below simply retries.
            newVal = Pack(
                static_cast<int32_t>(headRegion->GetCentralNextRegionIdx()),
                TagOf(oldVal) + 1);
        } while (!taggedHead_.compare_exchange_weak(oldVal, newVal,
                     std::memory_order_acq_rel, std::memory_order_acquire));
        headRegion->SetCentralNextRegionIdx(MSRegionInfo::INVALID_REGION_IDX);
        count_.fetch_sub(1, std::memory_order_relaxed);
        return headRegion;
    }

    // Publish a GC-private chain to this MPMC stack. The count is made visible
    // before the head so a concurrent Pop can never underflow the accounting.
    // The caller must own every node in chain.
    void PushChain(RegionChain&& chain)
    {
        if (chain.IsEmpty()) {
            return;
        }
        size_t chainCount = chain.count;
        int32_t chainHead = chain.headIdx;
        MSRegionInfo* chainTail = MSRegionInfoArray::GetRegionInfoByIdx(chain.tailIdx);
        count_.fetch_add(chainCount, std::memory_order_relaxed);
        uint64_t oldVal = taggedHead_.load(std::memory_order_acquire);
        uint64_t newVal;
        do {
            chainTail->SetCentralNextRegionIdx(static_cast<uint32_t>(IdxOf(oldVal)));
            newVal = Pack(chainHead, TagOf(oldVal) + 1);
        } while (!taggedHead_.compare_exchange_weak(
            oldVal, newVal, std::memory_order_release, std::memory_order_acquire));
        chain.Reset();
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

    // STW/quiescent ownership transfer to the GC-private collect-set.
    RegionChain StealChain()
    {
        uint64_t oldVal = taggedHead_.load(std::memory_order_relaxed);
        taggedHead_.store(
            Pack(MSRegionInfo::INVALID_IDX, TagOf(oldVal) + 1),
            std::memory_order_relaxed);
        size_t count = count_.exchange(0, std::memory_order_relaxed);
        RegionChain chain;
        chain.headIdx = IdxOf(oldVal);
        chain.count = count;
        int32_t idx = chain.headIdx;
        while (idx != MSRegionInfo::INVALID_IDX) {
            chain.tailIdx = idx;
            MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoByIdx(idx);
            idx = static_cast<int32_t>(info->GetCentralNextRegionIdx());
        }
        CHECK_DETAIL((chain.count == 0) == chain.IsEmpty(),
                     "[MS-GC] LockFreeRegionStack::StealChain count/head mismatch");
        return chain;
    }

    // Unsafe iteration for GC (STW only). Visitor must NOT modify the stack.
    template<typename Visitor>
    void ForEachUnsafe(Visitor&& visitor) const
    {
        int32_t idx = IdxOf(taggedHead_.load(std::memory_order_acquire));
        while (idx != MSRegionInfo::INVALID_IDX) {
            MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoByIdx(idx);
            int32_t next = static_cast<int32_t>(info->GetCentralNextRegionIdx());
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
// Final three-head design per (sizeClass, scanClass) lane:
//   partialSwept — swept, has allocation capacity
//   fullSwept    — swept, no allocation capacity
//   unswept      — current collect-set published by PostMark
//
// There is no Region-local sweep generation or sweep-state bit. Membership is
// the state, and a successful unswept Pop is the unique Sweep ownership claim.
//
class CentralRegionPool {
public:
    friend class AllocBufferMS;

    struct BucketPressure {
        size_t partialSwept = 0;
        size_t fullSwept = 0;
        size_t unswept = 0;

        size_t ReadyCount() const
        {
            return partialSwept;
        }

        size_t UnsweptCount() const
        {
            return unswept;
        }
    };

    explicit CentralRegionPool(MSRegionManager* msRegionManager);
    ~CentralRegionPool() = default;

    // ===== Thread-local Region acquisition =====

    // Fast path: take an already-swept partial region for the given size class.
    MSRegionInfo* TakePartialSweptRegion(uint8_t cls, MSScanClass scanClass);

    // Get a fresh mutator Region from RegionManager after central lists miss.
    // This path skips releasedTree and goes directly to bump/grow.
    MSRegionInfo* GetFreshRegion(
        uint8_t cls, MSScanClass scanClass, uint16_t shardHint = kMSInvalidShardId);
    uint16_t ResolveHomeShardId(uint16_t shardHint = kMSInvalidShardId) const;

    // ===== Thread-local Region return =====

    void ReturnPartialRegion(MSRegionInfo* region, uint8_t cls);
    void ReturnFullRegion(MSRegionInfo* region, uint8_t cls);
    void ReturnEmptyRegion(MSRegionInfo* region);

    // ===== GC operations =====

    // Background sweep: iterate all size classes, sweep one slice of unswept Regions.
    // Thread-safe (CAS ownership). Called by gc-helper thread.
    void BackgroundSweep(uint8_t preferredCls, uint64_t deadlineUs);
    void ParallelSweep(uint8_t preferredCls, uint64_t deadlineUs);
    // OOM PostMark-only sweep pass. All ordinary Sweep admission gates must remain
    // closed, and the caller must join every helper before reopening them. Each
    // worker visits every size class once and exits after observing an empty list;
    // residual unswept work is allowed and sweepDone_ is intentionally unchanged.
    void ParallelSweepOnceForOOM(uint8_t preferredCls);
    // Repay one P's local Sweep debt. Non-assist Sweep work is claimed once from
    // the central credit pool; a remaining full batch is swept directly once.
    MSSweepAssistResult MSSweepAssist(size_t debtRegions, uint8_t preferredCls);
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
        return IsSweepAdmissionOpen() &&
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
            sweepDone_.load(std::memory_order_acquire) &&
            (sweepAdmission_.load(std::memory_order_acquire) &
             kSweepAdmissionActiveMask) == 0;
    }

    // Visit every small Region currently owned by the central size-class lists.
    // The lock-free partial-swept list requires external quiescence or STW.
    template<typename Visitor>
    void VisitAllRegions(Visitor&& visitor);

    // STW-only mark-termination reconciliation domain. This is the complete
    // small-Region set that can retain a pending HAS_REF mark-word publication:
    // the immutable collect-set, central allocation supply, and TLS active
    // Regions. No Region task queue is introduced; callers inspect the
    // per-Region pending summary directly.
    void VisitMarkPendingRegionsUnderSTW(
        const std::function<void(MSRegionInfo*)>& visitor);

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
    size_t GetLargeObjectBytes() const
    {
        return largeLiveObjectBytes_.load(std::memory_order_acquire);
    }

    // Dump CentralRegionPool three-head statistics (block ⑤ in DFX design)
    void DumpPoolStats() const;

    static uint32_t GetSweepCycleId()
    {
        return sweepCycleId_.load(std::memory_order_acquire);
    }

    uint32_t GetCurrentMarkEpoch() const
    {
        return currentMarkEpoch_.load(std::memory_order_acquire);
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

    // OOM-only diagnostic serial closure. The regular OOM path uses
    // ParallelSweepOnceForOOM and does not require the unswept backlog to reach
    // zero. Keep this entry for explicit diagnostics and invariant checks.
    void ForceFinishAllUnsweptForOOM();

    // Clear mark state for central small Regions, thread-local active Regions,
    // and tracked large-object bases without scanning committed metadata.
    // Must be called after StopAndWaitForAllSweep().
    void ClearAllMarkBitmaps();

    // Build the immutable small-object collection boundary under PreEnum STW.
    // TLS active Regions stay outside the set. Returns the new non-zero epoch.
    uint32_t BuildCollectSet();

    // After Mark has reached a fixed point, transfer every collect-set chain to
    // the corresponding unswept supply. No Region is reclassified by its old
    // free-slot count before Sweep rebuilds the allocation plane.
    void PublishCollectSetForSweep();

    size_t GetCollectSetRegionCount() const;

    // Re-enable lazy/assist sweep after MarkSetup bitmap clearing is complete.
    void ReenableLazySweep();

private:
    class SizeClassBucket {
    public:
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

        MSRegionInfo* ClaimUnswept()
        {
            MSRegionInfo* claimed = unswept_.Pop();
            if (claimed != nullptr) {
                LeaveCentralList(false);
            }
            return claimed;
        }

        void PushPartialSwept(MSRegionInfo* region)
        {
            // Publish the aggregate before the lock-free list node. A concurrent
            // Pop may observe the node immediately and must never subtract from
            // an aggregate that has not recorded this Region yet.
            EnterCentralList(true);
            partialSwept_.Push(region);
        }

        void PushFullSwept(MSRegionInfo* region)
        {
            EnterCentralList(true);
            fullSwept_.Push(region);
        }

        void StealSweptToCollectSet(RegionChain& destination)
        {
            RegionChain partial = partialSwept_.StealChain();
            RegionChain full = fullSwept_.StealChain();
            size_t moved = partial.count + full.count;
            destination.Append(std::move(partial));
            destination.Append(std::move(full));
            SubAggregate(sweptRegions_, moved, "centralSmallSweptRegions",
                         std::memory_order_release);
        }

        void PublishCollectSetToUnswept(RegionChain& source)
        {
            size_t moved = source.count;
            AddAggregate(unsweptRegions_, moved, std::memory_order_release);
            unswept_.PushChain(std::move(source));
        }

        size_t CountPartialSwept() const
        {
            return partialSwept_.Count();
        }

        size_t CountFullSwept() const
        {
            return fullSwept_.Count();
        }

        size_t CountUnswept() const
        {
            return unswept_.Count();
        }

        BucketPressure GetPressure() const
        {
            BucketPressure pressure;
            pressure.partialSwept = CountPartialSwept();
            pressure.fullSwept = CountFullSwept();
            pressure.unswept = CountUnswept();
            return pressure;
        }

        template<typename Visitor>
        void VisitPartialSweptUnsafe(Visitor&& visitor) const
        {
            partialSwept_.ForEachUnsafe(visitor);
        }

        template<typename Visitor>
        void VisitAllUnsafe(Visitor&& visitor) const
        {
            partialSwept_.ForEachUnsafe(visitor);
            fullSwept_.ForEachUnsafe(visitor);
            unswept_.ForEachUnsafe(visitor);
        }

        template<typename FullSweptVisitor, typename UnsweptVisitor>
        void VisitNonPartialUnsafe(FullSweptVisitor&& fullSweptVisitor,
                                   UnsweptVisitor&& unsweptVisitor) const
        {
            fullSwept_.ForEachUnsafe(fullSweptVisitor);
            unswept_.ForEachUnsafe(unsweptVisitor);
        }

    private:
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
                // ClaimUnswept calls this only after uniquely removing the node.
                SubAggregate(unsweptRegions_, 1, "centralSmallUnsweptRegions",
                             std::memory_order_acq_rel);
            }
        }

        std::atomic<size_t>* activeRegions_ = nullptr;
        std::atomic<size_t>* sweptRegions_ = nullptr;
        std::atomic<size_t>* unsweptRegions_ = nullptr;
        LockFreeRegionStack partialSwept_;
        LockFreeRegionStack fullSwept_;
        LockFreeRegionStack unswept_;
    };

    void PushEmptySweptRegion(MSRegionInfo* region);
    MSRegionInfo* TakeGlobalEmptyRegion();
    size_t DrainEmptySweptGlobal(size_t maxRegions, std::vector<MSRegionInfo*>& drained);
    void ForceFinishUnsweptList(SizeClassBucket& bucket,
                                size_t& sweptRegions, size_t& emptiedRegions,
                                size_t& retainedRegions, size_t& freedBytes);
    void ForceFinishAllUnswept();
    bool TryEnterSweep();
    void LeaveSweep();
    void CloseSweepAdmissionAndWait();
    void OpenSweepAdmission();
    void PublishSweepDoneAfterQuiescence();
    bool IsSweepAdmissionOpen() const
    {
        return (sweepAdmission_.load(std::memory_order_acquire) & kSweepAdmissionClosed) == 0;
    }
    size_t AssistSweep(size_t targetRegions, uint8_t preferredCls);
    bool SweepOneUnsweptRegionForAssist(
        SizeClassBucket& bucket, size_t* emptiedInSlice = nullptr);
    bool SweepUnsweptList(SizeClassBucket& bucket, size_t& sweptInSlice,
                         size_t maxRegions, uint64_t deadlineUs,
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
    static constexpr uint64_t kSweepAdmissionClosed = 1ULL << 63;
    static constexpr uint64_t kSweepAdmissionActiveMask = ~kSweepAdmissionClosed;

    static std::atomic<uint32_t> sweepCycleId_;
    std::atomic<bool> sweepDone_{true};
    std::atomic<bool> bgSweepRunning_{false};
    // One atomic word linearizes sweep admission with the active-worker count.
    // The high bit closes admission; the remaining bits count all admitted
    // lazy/assist/helper/background sweep operations. This avoids a late
    // counter increment after StopAndWaitForAllSweep has observed quiescence.
    std::atomic<uint64_t> sweepAdmission_{0};
    std::atomic<bool> stopSweepRequested_{false};
    std::atomic<bool> skipLazySweep_{false};          // Disable lazy sweep in GetRegion P2/P3
    std::atomic<bool> skipAssistSweep_{false};        // Disable mutator assist sweep before STW
    std::atomic<bool> skipHelperSweep_{false};        // Disable GC-helper pool sweep before STW
    std::atomic<size_t> activeLazySweepCount_{0};     // In-flight lazy sweep counter
    std::atomic<size_t> activeAssistSweepCount_{0};   // In-flight mutator assist sweep counter
    std::atomic<size_t> activeHelperSweepCount_{0};   // In-flight parallel helper sweep counter
    std::atomic<bool> forceFinishSweepRunning_{false}; // GC main owns ForceFinishAllUnswept
    // ===== P-local proportional Sweep pacing =====
    // PostMark publishes one fixed-point ratio for the new sweep cycle.
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
    SizeClassBucket buckets_[kNumSizeClasses][kMSScanClassCount];
    RegionChain collectSet_[kNumSizeClasses][kMSScanClassCount];
    std::atomic<uint32_t> currentMarkEpoch_{0};
    MSRegionManager*  msRegionManager_;

    // Large region tracking (Tier 1: in-heap large objects, MS_REGION_LARGE).
    // Only primary Region indexes are stored. PreEnum swaps next-supply into
    // the immutable collect-set; allocations after the boundary append to the
    // now-empty next-supply vector.
    std::mutex largeLock_;
    std::vector<uint32_t> largeNextSupply_;
    std::vector<uint32_t> largeCollectSet_;
    std::atomic<size_t> largeLiveSpanCount_{0};
    std::atomic<size_t> largeLiveObjectBytes_{0};
    void AddLargeLiveRegion(MSRegionInfo* region);
    void CollectDeadLargeRegions(uint32_t collectEpoch,
                                 std::vector<MSRegionInfo*>& deadRegions);
    template<typename Visitor>
    void VisitLargeRegionsLocked(Visitor&& visitor) const
    {
        std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(largeLock_));
        for (uint32_t idx : largeCollectSet_) {
            visitor(MSRegionInfoArray::GetRegionInfoByIdx(idx));
        }
        for (uint32_t idx : largeNextSupply_) {
            visitor(MSRegionInfoArray::GetRegionInfoByIdx(idx));
        }
    }
    template<typename Visitor>
    void VisitLargeCollectSetLocked(Visitor&& visitor) const
    {
        std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(largeLock_));
        for (uint32_t idx : largeCollectSet_) {
            visitor(MSRegionInfoArray::GetRegionInfoByIdx(idx));
        }
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

    // Global cache of classless empty regions. All mutators refill directly from
    // this queue; no per-P spare EmptyRegion cache is retained.
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
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            SizeClassBucket& bucket = buckets_[cls][scan];
            bucket.VisitAllUnsafe(visitor);
        }
    }
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_CENTRAL_REGION_POOL_H
