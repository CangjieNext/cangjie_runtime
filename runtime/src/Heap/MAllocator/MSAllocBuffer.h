// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_ALLOC_BUFFER_H
#define MRT_MS_ALLOC_BUFFER_H

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/Allocator/RegionInfo.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSSizeClass.h"
#include "Common/TypeDef.h"
#include "Common/MarkWorkStack.h"
#include "Common/LFStack.h"
#include <atomic>
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
    static AllocBufferMS* GetOrCreateAllocBufferMS(ThreadLocalData* local, CentralRegionPool* central);
    static AllocBufferMS* GetAllocBufferMS();

    AllocBufferMS();

    // LLVM fast-path stub: offset 0 must be a RegionInfo* (same layout as AllocBuffer::tlRegion).
    // Initialized to NullRegion() so the compiler-generated fast path reads it,
    // sees allocPtr >= regionEnd (both 0), and falls through to the slow MCC_* call.
    // This is necessary because SDK standard libraries may be compiled with fast-path enabled.
    RegionInfo* GetTlRegion() const { return tlRegion_; }
    ~AllocBufferMS();

    void Init(CentralRegionPool* central);
    void Fini();

    // Main allocation entry (called from AllocBuffer::Allocate in MS mode)
    // Reuse caller-resolved ThreadLocalData when available to avoid extra TLS lookups
    // on the mutator allocation path.
    MAddress Allocate(size_t size, ThreadLocalData* local = nullptr);

    // Accumulate pacing debt for a given allocation size and trigger assist if needed.
    // Called by large-object paths that bypass Allocate().
    void AccumulatePacingDebt(size_t size);


    // GC STW: flush all thread-local Regions back to CentralRegionPool
    void FlushAll();

    // Background: fill empty cachedRegion_ slots for size classes with high miss pressure.
    // Called by gc-helper thread (FinalizerProcessor). Thread-safe via atomic cachedRegion_.
    void BackgroundFillCache();

    // Record stack root (called from Mutator::GcPhaseEnum during root enumeration).
    // This is root staging only, not grey-work production. Objects become grey
    // later when MarkSweepCollector merges these buffered roots through
    // TryEnqueueGreyObject / DiscoverAndPushObject.
    void PushRoot(BaseObject* root) { stackRoots_.emplace_back(root); }

    // MS-GC: all objects (including RAW_POINTER_OBJECT) are managed by CentralRegionPool directly.
    // No separate thread-local raw pointer regions exist, so this is a no-op.
    void CommitRawPointerRegions() {}

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
    MSRegionInfo* GetActiveRegion(uint8_t cls)
    {
        if (cls >= kNumSizeClasses) {
            return nullptr;
        }
        SyncActiveBumpRegionShadow(cls);
        return activeRegions_[cls];
    }

    // Per-P GC Work buffer (Step5): used by Mutator assist during concurrent mark.
    // Initialized by MarkSweepCollector::InitAllPerPGCWork() at GC start.
    PerPGCWork& GetGCWork() { return gcWork_; }

private:
    static constexpr uint8_t kCachedRegionSlots = 2;

    enum class ActiveAllocKind : uint8_t {
        NONE = 0,
        BUMP,
        BITMAP,
    };

    // LLVM fast-path compatibility: must be at offset 0 (same as AllocBuffer::tlRegion).
    // Set to NullRegion() so fast-path allocation always fails → slow path.
    RegionInfo* tlRegion_;

    // Fast path: try bitmap allocation on current active Region
    MAddress FastAlloc(uint8_t cls);

    // Slow path: current Region exhausted, refill from CentralRegionPool
    MAddress SlowAlloc(uint8_t cls);

    // Install a new active region and initialize any fast-path shadow state.
    void SetActiveRegion(uint8_t cls, MSRegionInfo* region);

    // Sync local bump-allocation shadows back to the active region metadata.
    void SyncActiveBumpRegionShadow(uint8_t cls);

    // Refill: get a Region from CentralRegionPool
    MSRegionInfo* RefillFromCentral(uint8_t cls);

    // Consume one cached backup Region for the given size class.
    MSRegionInfo* TakeCachedRegion(uint8_t cls);

    // Try to store a backup Region into one empty cache slot.
    bool TryCacheRegion(uint8_t cls, MSRegionInfo* region);

    // Return a cached/active region back to the central pool using the same
    // classification as ReturnRegion.
    void ReturnRegionToCentral(MSRegionInfo* region, uint8_t cls);

    // Return current active Region to CentralRegionPool
    void ReturnRegion(uint8_t cls);

    // Per-size-class active Region (nullptr = no active Region for this class)
    MSRegionInfo* activeRegions_[kNumSizeClasses] = {};

    // Cache the active allocation strategy so FastAlloc does not need to branch
    // on regionType for every small-object allocation.
    ActiveAllocKind activeAllocKinds_[kNumSizeClasses] = {};

    // For fresh BUMP regions, keep allocPtr/regionEnd in the AllocBuffer so the
    // mutator fast path can avoid touching MSRegionInfo metadata on every hit.
    uintptr_t activeAllocPtrs_[kNumSizeClasses] = {};
    uintptr_t activeRegionEnds_[kNumSizeClasses] = {};

    // O2/O9: Per-size-class cached backup Regions.
    // Written by gc-helper (BackgroundFillCache), consumed by mutator (SlowAlloc).
    // Two slots reduce synchronous trips to CentralRegionPool on bursty refill workloads.
    std::atomic<MSRegionInfo*> cachedRegion_[kNumSizeClasses][kCachedRegionSlots] = {};

    // O9: Per-size-class cache miss counter. Mutator increments on SlowAlloc
    // when cachedRegion_ is empty. Background thread fills when >= threshold.
    static constexpr uint8_t kCacheFillThreshold = 2;
    uint8_t cacheMiss_[kNumSizeClasses] = {};

    // Back-pointer to global CentralRegionPool
    CentralRegionPool* central_ = nullptr;

    // Per-thread stack roots buffer (populated by Mutator::GcPhaseEnum)
    std::vector<BaseObject*> stackRoots_;

    // Mutator assist debt in fixed-point "object" units.
    // debtScaled / AssistRatioScale = debtObjects.
    uint64_t gcAssistDebtScaled_ = 0;

    // Step5: Per-P GC work buffer for mutator assist
    PerPGCWork gcWork_;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_ALLOC_BUFFER_H
