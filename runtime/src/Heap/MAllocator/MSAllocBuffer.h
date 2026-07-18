// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_ALLOC_BUFFER_H
#define MRT_MS_ALLOC_BUFFER_H

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/Allocator/RegionInfo.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSSizeClass.h"
#include "Heap/MCollector/MSMarkLocalStats.h"
#include "Common/TypeDef.h"
#include "Common/MarkWorkStack.h"
#include "Common/LFStack.h"
#include <atomic>
#include <mutex>
#include <vector>

namespace MapleRuntime {

class CentralRegionPool;
class Mutator;
struct ThreadLocalData;

// ============================================================================
// AllocBufferMS: per-thread allocation cache for Mark-Sweep GC
// ============================================================================
//
// Go equivalent: mcache — one per P (Processor), holding one active mspan
// per size class. Our design: one AllocBufferMS per thread/coroutine Processor,
// holding one active MSRegionInfo per size class (36 slots).
//
// Allocation path (Phase 1 — all allocations go through runtime call):
//   AllocBufferMS::Allocate(size)
//     → SizeToClass(size)
//     → FastAlloc(cls): bitmap CTZ on activeRegion
//       → hit: return address
//       → miss: SlowAlloc(cls): refill from CentralRegionPool
//
// Phase 2 (future): compiler inlines bitmap CTZ fast-path, only SlowAlloc
// goes through runtime. See Step1.2 design doc §2.6.
//
class AllocBufferMS {
public:
    // Mirror of AllocBuffer::GetOrCreateAllocBuffer / GetAllocBuffer for MS mode.
    static AllocBufferMS* GetOrCreateAllocBufferMS();
    static AllocBufferMS* GetOrCreateAllocBufferMS(ThreadLocalData* local);
    static AllocBufferMS* GetAllocBufferMS();

    AllocBufferMS();

    // LLVM fast-path stub: offset 0 must be a RegionInfo* (same layout as AllocBuffer::tlRegion).
    // Initialized to NullRegion() so the compiler-generated fast path reads it,
    // sees allocPtr >= regionEnd (both 0), and falls through to the slow MCC_* call.
    // This is necessary because SDK standard libraries may be compiled with fast-path enabled.
    ~AllocBufferMS();

    void Init(CentralRegionPool* central);
    void Fini();

    // Main allocation entry (called from AllocBuffer::Allocate in MS mode)
    MAddress Allocate(size_t size);

    // Accumulate pacing debt for a given allocation size and trigger assist if needed.
    // Called by large-object paths that bypass Allocate().
    void AccumulatePacingDebt(size_t size);

    // Charge successful Region consumption to this P and repay its local Sweep
    // debt with one-shot central credit followed by direct assist work.
    void AccumulateSweepDebt(size_t allocatedRegions, uint8_t preferredCls);

    uint32_t GetSweepDebtGeneration() const
    {
        return sweepDebtGen_.load(std::memory_order_acquire);
    }

    MSSweepAssistStats SnapshotSweepAssistStats() const
    {
        return sweepAssistStats_.Snapshot();
    }

    // Record stack root (called from Mutator::GcPhaseEnum during root enumeration).
    // This is root staging only, not grey-work production. Objects become grey
    // later when MarkSweepCollector merges these buffered roots through
    // TryEnqueueGreyObject / DiscoverAndPushObject.
    void PushRoot(BaseObject* root) { stackRoots_.emplace_back(root); }

    // Move buffered stack roots into a temporary root buffer.
    // This helper intentionally does not discover/mark/enqueue grey work; the
    // collector owns the later white->grey transition when it drains the merged
    // root buffer into its grey-work queues.
    template<class WorkStack>
    inline void MergeRoots(WorkStack& workStack)
    {
        if (stackRoots_.empty()) {
            return;
        }
        for (BaseObject* obj : stackRoots_) {
            workStack.push_back(obj);
        }
        stackRoots_.clear();
    }

    // Get active Region for a given size class (for GC iteration)
    MSRegionInfo* GetActiveRegion(uint8_t cls) const
    {
        if (cls >= kNumSizeClasses) {
            return nullptr;
        }
        return activeRegions_[cls];
    }

    size_t CountActiveRegions() const;
    size_t ReturnAllRegionsToCentral();

    // Per-P GC Work buffer (Step5): used by Mutator assist during concurrent mark.
    // Initialized by MarkSweepCollector::InitAllPerPGCWork() at GC start.
    PerPGCWork& GetGCWork() { return gcWork_; }
    MSMarkAssistStats& GetMarkAssistStats() { return markAssistStats_; }

    // Trusted-grey write-barrier log accessors.
    WorkBuf* GetActiveLog() const { return activeLog_; }
    WorkBuf*& ActiveLogRef() { return activeLog_; }

    // Acquire an empty WorkBuf, per-P freelist first (no lock), batch-refilling
    // from the global pool on miss. Returned buffers are reset.
    WorkBuf* AcquireWorkBuf(WorkBufPool& pool)
    {
        if (logCache_ != nullptr) {
            WorkBuf* b = logCache_;
            logCache_ = b->next;
            b->next = nullptr;
            --logCacheCount_;
            b->Reset();
            return b;
        }
        WorkBuf* bufs[kLogCacheRefill];
        size_t got = pool.GetN(bufs, kLogCacheRefill);
        if (got == 0) {
            return nullptr;
        }
        for (size_t i = 1; i < got; ++i) {
            bufs[i]->next = logCache_;
            logCache_ = bufs[i];
            ++logCacheCount_;
        }
        bufs[0]->Reset();
        return bufs[0];
    }

    // Return every cached WorkBuf to the global pool. Called at GC cycle end.
    void DrainLogCacheToPool(WorkBufPool& pool)
    {
        while (logCache_ != nullptr) {
            WorkBuf* t = logCache_;
            logCache_ = t->next;
            t->next = nullptr;
            pool.Put(t);
        }
        logCacheCount_ = 0;
    }

    uint16_t GetCachedHomeShardId() const { return cachedHomeShardId_; }

private:
    friend class CentralRegionPool;
    static constexpr size_t kLocalEmptyRegionCap = kNumSizeClasses;
    // Per-P WorkBuf log cache tuning (Step37 §5.1).
    static constexpr size_t kLogCacheRefill = 8;   // batch size to pull from global pool

    // LLVM fast-path compatibility: must be at offset 0 (same as AllocBuffer::tlRegion).
    // Set to NullRegion() so fast-path allocation always fails → slow path.
    RegionInfo* tlRegion_;

    // Fast path: try bitmap allocation on current active Region
    MAddress FastAlloc(uint8_t cls);

    // Slow path: current Region exhausted, refill from CentralRegionPool
    MAddress SlowAlloc(uint8_t cls);

    // Refill: get a Region from CentralRegionPool
    MSRegionInfo* RefillFromCentral(uint8_t cls);
    MSRegionInfo* GetCacheRegion(uint8_t cls);
    MSRegionInfo* TakeLocalEmptyRegion();
    bool TryPushLocalEmptyRegion(MSRegionInfo* region, size_t slotLimit = kLocalEmptyRegionCap);
    void RefillLocalEmptyRegionsFromGlobal(std::vector<MSRegionInfo*>& refill);
    size_t DrainLocalEmptyRegions(std::vector<MSRegionInfo*>& out, size_t maxRegions);
    size_t DrainAllLocalEmptyRegions(std::vector<MSRegionInfo*>& out);
    bool HasLocalEmptyRegionInRange(size_t startRegionIdx, size_t endRegionIdx) const;
    size_t ExtractLocalEmptyRegionsInRange(std::vector<MSRegionInfo*>& out, size_t startRegionIdx,
                                           size_t endRegionIdx, size_t maxRegions);

    // Return an active region back to the central pool using the same
    // classification as ReturnRegion.
    void ReturnRegionToCentral(MSRegionInfo* region, uint8_t cls);

    // Return current active Region to CentralRegionPool
    void ReturnRegion(uint8_t cls);

    // Per-size-class active Region (nullptr = no active Region for this class)
    MSRegionInfo* activeRegions_[kNumSizeClasses] = {};

    // Per-AllocBuffer cache of classless empty regions. These shells are rebound
    // to a size class on demand after the typed central lists miss.
    mutable std::mutex localEmptyRegionLock_;
    MSRegionList localEmptyRegionCache_;

    // Back-pointer to global CentralRegionPool
    CentralRegionPool* central_ = nullptr;

    // Cached home shard id for this per-P buffer.
    // Resolved once on first TakeRegion call and reused thereafter.
    // Safe to cache because AllocBufferMS is per-OS-thread (per-P).
    uint16_t cachedHomeShardId_ = kMSInvalidShardId;

    // Per-thread stack roots buffer (populated by Mutator::GcPhaseEnum)
    std::vector<BaseObject*> stackRoots_;

    // Mutator assist debt in fixed-point "object" units.
    // debtScaled / AssistRatioScale = debtObjects.
    uint64_t gcAssistDebtScaled_ = 0;

    // Preserve the historical AArch64 MSGC fast-path ABI offset (896) for the
    // write-barrier log slot. The slot is a pointer to a WorkBuf (activeLog_),
    // not an inline buffer. The AArch64 fast path loads
    // this pointer, then writes the old-ref into activeLog_->objs[len++].
    // The removed per-size-class typed-spare array remains reserved padding so
    // activeLog_ keeps its compiler/runtime ABI offset.
    uint8_t wbBufAbiReserved_[184 +
        kNumSizeClasses * sizeof(std::atomic<MSRegionInfo*>)] = {};

    // The barrier appends already shaded old-refs into *activeLog_. When full,
    // it is published wholesale to the GC work queues (swap-and-publish) and
    // replaced from logCache_. Consumers scan these trusted-grey objects
    // directly. Kept at offset 896 for the fast path.
    WorkBuf* activeLog_ = nullptr;      // offset 896 (fast-path ABI)

    // Per-P freelist of empty WorkBufs (single-owner, lock-free). Backs
    // AcquireWorkBuf so the hot swap path avoids the global
    // WorkBufPool mutex on most acquisitions. Drained to the pool at cycle end.
    WorkBuf* logCache_ = nullptr;
    size_t   logCacheCount_ = 0;

    // Step5: Per-P GC work buffer for mutator assist
    PerPGCWork gcWork_;

    // Single-owner assist diagnostics. The collector aggregates these after
    // stopping mutators, so the assist hot path needs no shared atomics.
    MSMarkAssistStats markAssistStats_;

    // Per-P Sweep pacing. Debt remains single-owner; generation and diagnostics
    // are atomic because the closing-cycle REPORT is emitted just before STW and
    // may snapshot a still-running P. Keep this block after activeLog_/gcWork_ so
    // the AArch64 write-barrier ABI offset remains unchanged.
    uint64_t sweepAssistDebtScaled_ = 0;
    std::atomic<uint32_t> sweepDebtGen_ {0};
    MSSweepAssistAtomicStats sweepAssistStats_;

};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_ALLOC_BUFFER_H
