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
#include "Heap/MAllocator/MSScanClass.h"
#include "Heap/MAllocator/MSSizeClass.h"
#include "Heap/MCollector/MSMarkLocalStats.h"
#include "Common/TypeDef.h"
#include <new>
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
    MAddress Allocate(size_t size, MSScanClass scanClass);

    // Accumulate pacing debt for a given allocation size and trigger assist if needed.
    // Called by large-object paths that bypass Allocate().
    void AccumulatePacingDebt(size_t size);

    // Charge successful Region consumption to this P and repay its local Sweep
    // debt with one-shot central credit followed by direct assist work.
    void AccumulateSweepDebt(size_t allocatedRegions, uint8_t preferredCls);

    // Record one stack root during the Mutator safepoint. The reserved vector
    // is the common path; overflow uses fixed native chunks retained by this
    // AllocBufferMS across cycles. This is staging, not grey-work production.
    void PushRoot(BaseObject* root);

    // Move buffered stack roots into a temporary root buffer.
    // This helper intentionally does not discover/mark/enqueue grey work; the
    // collector owns the later white->grey transition when it drains the merged
    // root staging buffer into MarkWordBatch.
    template<class WorkStack>
    inline void MergeRoots(WorkStack& workStack)
    {
        for (BaseObject* obj : stackRoots_) {
            workStack.push_back(obj);
        }
        stackRoots_.clear();

        RootOverflowChunk* chunk = rootOverflowHead_;
        rootOverflowHead_ = nullptr;
        rootOverflowTail_ = nullptr;
        while (chunk != nullptr) {
            for (size_t i = 0; i < chunk->count; ++i) {
                workStack.push_back(chunk->roots[i]);
            }
            RootOverflowChunk* next = chunk->next;
            chunk->count = 0;
            chunk->next = rootOverflowFree_;
            rootOverflowFree_ = chunk;
            chunk = next;
        }
    }

    bool IsRootStagingEmpty() const
    {
        return stackRoots_.empty() && rootOverflowHead_ == nullptr;
    }

    // Get active Region for a given size class (for GC iteration)
    MSRegionInfo* GetActiveRegion(uint8_t cls, MSScanClass scanClass) const
    {
        if (cls >= kNumSizeClasses) {
            return nullptr;
        }
        return activeRegions_[cls][ToMSScanClassIndex(scanClass)];
    }

    size_t CountActiveRegions() const;
    size_t ReturnAllRegionsToCentral();

    MSMarkAssistStats& GetMarkAssistStats() { return markAssistStats_; }

    uint16_t GetCachedHomeShardId() const { return cachedHomeShardId_; }

private:
    static constexpr size_t kRootStagingReserve = 256;
    static constexpr size_t kRootOverflowChunkCapacity = 128;

    struct RootOverflowChunk {
        RootOverflowChunk* next = nullptr;
        size_t count = 0;
        BaseObject* roots[kRootOverflowChunkCapacity] = {};
    };

    RootOverflowChunk* AcquireRootOverflowChunk();
    void DestroyRootOverflowChunks();

    // Fast path: try bitmap allocation on current active Region
    MAddress FastAlloc(uint8_t cls, MSScanClass scanClass);

    // Slow path: current Region exhausted, refill from CentralRegionPool
    MAddress SlowAlloc(uint8_t cls, MSScanClass scanClass);

    // Refill: get a Region from CentralRegionPool
    MSRegionInfo* RefillFromCentral(uint8_t cls, MSScanClass scanClass);

    // Return an active region back to the central pool using the same
    // classification as ReturnRegion.
    void ReturnRegionToCentral(MSRegionInfo* region, uint8_t cls);

    // Return current active Region to CentralRegionPool
    void ReturnRegion(uint8_t cls, MSScanClass scanClass);

    // Compiler-visible fixed ABI prefix. ScanClass is the inner dimension:
    // activeRegions_[sizeClass][NO_REF/HAS_REF].
    MSRegionInfo* activeRegions_[kNumSizeClasses][kMSScanClassCount] = {};

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
    RootOverflowChunk* rootOverflowHead_ = nullptr;
    RootOverflowChunk* rootOverflowTail_ = nullptr;
    RootOverflowChunk* rootOverflowFree_ = nullptr;

    // Mutator assist debt in fixed-point WorkUnit units.
    // debtScaled / AssistRatioScale = debtWU.
    uint64_t gcAssistDebtScaled_ = 0;

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
