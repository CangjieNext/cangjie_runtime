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

    uint16_t GetCachedHomeShardId() const { return cachedHomeShardId_; }

private:
    // LLVM fast-path compatibility: must be at offset 0 (same as AllocBuffer::tlRegion).
    // Set to NullRegion() so fast-path allocation always fails → slow path.
    RegionInfo* tlRegion_;

    // Fast path: try bitmap allocation on current active Region
    MAddress FastAlloc(uint8_t cls);

    // Slow path: current Region exhausted, refill from CentralRegionPool
    MAddress SlowAlloc(uint8_t cls);

    // Refill: get a Region from CentralRegionPool
    MSRegionInfo* RefillFromCentral(uint8_t cls);

    // Return an active region back to the central pool using the same
    // classification as ReturnRegion.
    void ReturnRegionToCentral(MSRegionInfo* region, uint8_t cls);

    // Return current active Region to CentralRegionPool
    void ReturnRegion(uint8_t cls);

    // Per-size-class active Region (nullptr = no active Region for this class)
    MSRegionInfo* activeRegions_[kNumSizeClasses] = {};

    // Stable compiler/runtime ABI prefix. AArch64 and X86 load activeLog_ from
    // AllocBufferMS + 896. Keep this prefix independent of std::mutex,
    // std::vector, and every other platform/STL-dependent type.
    static constexpr size_t kActiveLogOffset = 896;
    static_assert(kActiveLogOffset >= sizeof(RegionInfo*) +
        kNumSizeClasses * sizeof(MSRegionInfo*),
        "AllocBufferMS fixed fast-path prefix exceeds activeLog_ offset");
    uint8_t activeLogAbiReserved_[kActiveLogOffset - sizeof(RegionInfo*) -
        kNumSizeClasses * sizeof(MSRegionInfo*)] = {};

    // The barrier appends already shaded old-refs into *activeLog_. When full,
    // it is published wholesale to the GC work queues (swap-and-publish) and
    // replaced directly from the collector-global WorkBufPool. Consumers scan
    // these trusted-grey objects directly. Kept at offset 896 for the
    // AArch64/X86 fast-path ABI.
    WorkBuf* activeLog_ = nullptr;

    // Runtime-only fields start after the compiler-visible fast-path prefix.
    // Their offsets may follow the target C++ ABI and are never embedded in
    // AArch64/X86 generated code.

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

    // Step5: Per-P GC work buffer for mutator assist
    PerPGCWork gcWork_;

    // Single-owner assist diagnostics. The collector aggregates these after
    // stopping mutators, so the assist hot path needs no shared atomics.
    MSMarkAssistStats markAssistStats_;

    // Per-P Sweep pacing. Both fields are single-owner and remain outside the
    // compiler-visible fixed ABI prefix.
    uint64_t sweepAssistDebtScaled_ = 0;
    uint32_t sweepDebtGen_ = 0;

};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_ALLOC_BUFFER_H
