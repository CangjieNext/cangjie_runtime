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
#include <atomic>
#include <vector>

namespace MapleRuntime {

class CentralRegionPool;

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
    MAddress Allocate(size_t size);

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
    MSRegionInfo* GetActiveRegion(uint8_t cls) const
    {
        if (cls >= kNumSizeClasses) {
            return nullptr;
        }
        return activeRegions_[cls];
    }

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

    // Return current active Region to CentralRegionPool
    void ReturnRegion(uint8_t cls);

    // Per-size-class active Region (nullptr = no active Region for this class)
    MSRegionInfo* activeRegions_[kNumSizeClasses] = {};

    // O2/O9: Per-size-class cached backup Region.
    // Written by gc-helper (BackgroundFillCache), consumed by mutator (SlowAlloc).
    // Atomic: mutator exchange→nullptr, gc-helper CAS nullptr→region.
    std::atomic<MSRegionInfo*> cachedRegion_[kNumSizeClasses] = {};

    // O9: Per-size-class cache miss counter. Mutator increments on SlowAlloc
    // when cachedRegion_ is empty. Background thread fills when >= threshold.
    static constexpr uint8_t kCacheFillThreshold = 2;
    uint8_t cacheMiss_[kNumSizeClasses] = {};

    // Back-pointer to global CentralRegionPool
    CentralRegionPool* central_ = nullptr;

    // Per-thread stack roots buffer (populated by Mutator::GcPhaseEnum)
    std::vector<BaseObject*> stackRoots_;

    // Mutator assist: accumulated allocation debt (bytes).
    // When debt exceeds kGCAssistTriggerBytes, triggers MSMarkAssist().
    static constexpr size_t kGCAssistTriggerBytes = 65536;  // 64KB
    size_t gcAssistBytes_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_ALLOC_BUFFER_H
