// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ALLOC_BUFFER_MS_H
#define MRT_ALLOC_BUFFER_MS_H

#ifdef MRT_USE_MARK_SWEEP

#include "MSRegionInfo.h"
#include "SizeClass.h"
#include "Common/TypeDef.h"
#include "Common/MarkWorkStack.h"
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
    AllocBufferMS() = default;
    ~AllocBufferMS();

    void Init(CentralRegionPool* central);
    void Fini();

    // Main allocation entry (called from AllocBuffer::Allocate in MS mode)
    MAddress Allocate(size_t size);

    // GC STW: flush all thread-local Regions back to CentralRegionPool
    void FlushAll();

    // Record stack root (called from Mutator::GcPhaseEnum during root enumeration)
    void PushRoot(BaseObject* root) { stackRoots_.emplace_back(root); }

    // Move buffered stack roots into GC work stack (called from MergeMutatorRoots)
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

    // O2: Per-size-class cached backup Region (prefetched in SlowAlloc)
    MSRegionInfo* cachedRegion_[kNumSizeClasses] = {};

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
#endif // MRT_ALLOC_BUFFER_MS_H
