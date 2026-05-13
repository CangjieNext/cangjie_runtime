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
#include "Common/TypeDef.h"
#include "Common/MarkWorkStack.h"
#include "Common/LFStack.h"
#include <atomic>
#include <vector>

namespace MapleRuntime {

void MSAllocBuffer_RecordCentralSweepNotify();
extern "C" void MSAllocBuffer_DumpAndResetSlowAllocCounters();

struct MSAllocCacheFillStats {
    size_t filledClasses = 0;
    size_t filledRegions = 0;

    bool HasWork() const
    {
        return filledClasses != 0 || filledRegions != 0;
    }
};

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
    enum class HotClassLevel : uint8_t {
        NONE = 0,
        SECONDARY,
        PRIMARY,
    };

    // Mirror of AllocBuffer::GetOrCreateAllocBuffer / GetAllocBuffer for MS mode.
    static AllocBufferMS* GetOrCreateAllocBufferMS();
    static AllocBufferMS* GetOrCreateAllocBufferMS(ThreadLocalData* local);
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
    MAddress Allocate(size_t size, ThreadLocalData* local = nullptr);

    // Accumulate pacing debt for a given allocation size and trigger assist if needed.
    // Called by large-object paths that bypass Allocate().
    void AccumulatePacingDebt(size_t size);


    // GC STW: flush all thread-local Regions back to CentralRegionPool
    void FlushAll();

    // Background: fill empty cachedRegion_ slots for size classes with high miss pressure.
    // Called by gc-helper thread (FinalizerProcessor). Thread-safe via atomic cachedRegion_.
    MSAllocCacheFillStats BackgroundFillCache(uint8_t preferredCls, bool reclaimPressure,
                                             MSFeedPressureLevel pressureLevel);

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

    size_t CountActiveRegions() const;
    size_t CountCachedRegions() const;
    uint16_t SnapshotHotClassScore(uint8_t cls) const { return GetHotClassScore(cls); }
    HotClassLevel SnapshotHotClassLevel(uint8_t cls) const
    {
        if (cls >= kNumSizeClasses) {
            return HotClassLevel::NONE;
        }
        if (IsPrimaryHotClass(cls)) {
            return HotClassLevel::PRIMARY;
        }
        if (IsSecondaryHotClass(cls)) {
            return HotClassLevel::SECONDARY;
        }
        return HotClassLevel::NONE;
    }
    size_t CountTotalRegions() const { return CountActiveRegions() + CountCachedRegions(); }

    // Per-P GC Work buffer (Step5): used by Mutator assist during concurrent mark.
    // Initialized by MarkSweepCollector::InitAllPerPGCWork() at GC start.
    PerPGCWork& GetGCWork() { return gcWork_; }

private:
    friend class CentralRegionPool;
    static constexpr uint8_t kNoPreferredHotClass = UINT8_MAX;
    static constexpr uint8_t kCachedRegionSlots = 5;
    static constexpr uint8_t kBaseCachedRegionSlots = 2;

    // LLVM fast-path compatibility: must be at offset 0 (same as AllocBuffer::tlRegion).
    // Set to NullRegion() so fast-path allocation always fails → slow path.
    RegionInfo* tlRegion_;

    // Fast path: try bitmap allocation on current active Region
    MAddress FastAlloc(uint8_t cls);

    // Slow path: current Region exhausted, refill from CentralRegionPool
    MAddress SlowAlloc(uint8_t cls);

    // Refill: get a Region from CentralRegionPool
    MSRegionInfo* RefillFromCentral(uint8_t cls, bool backgroundFill = false);

    uint8_t BumpCacheMiss(uint8_t cls);
    void SetCacheMiss(uint8_t cls, uint8_t value);
    void BoostCacheHeat(uint8_t cls, uint8_t delta);
    void CoolCacheHeat(uint8_t cls, uint8_t delta);
    uint8_t CountCachedRegions(uint8_t cls) const;
    uint16_t GetHotClassScore(uint8_t cls) const;
    uint8_t GetHotClassRank(uint8_t cls) const;
    uint8_t GetHotHintClass() const;
    bool IsPrimaryHotClass(uint8_t cls) const;
    bool IsSecondaryHotClass(uint8_t cls) const;
    bool ShouldNotifyBackgroundFeed(uint8_t cls) const;
    bool ShouldBackgroundFillClass(uint8_t cls, uint8_t preferredCls,
                                   MSFeedPressureLevel pressureLevel) const;
    uint8_t GetTargetCachedSlots(uint8_t cls, bool reclaimPressure = false,
                                 MSFeedPressureLevel pressureLevel = MSFeedPressureLevel::LOW) const;
    uint8_t GetCacheFillThreshold(uint8_t cls) const;
    uint16_t GetBackgroundFillPriority(uint8_t cls) const;
    void NotifyBackgroundFillIfNeeded(uint8_t cls);
    void TrimExtraCachedRegions(uint8_t cls, uint8_t targetSlots = kBaseCachedRegionSlots);

    // Consume one cached backup Region for the given size class.
    MSRegionInfo* TakeCachedRegion(uint8_t cls);

    // Try to store a backup Region into one empty cache slot.
    bool TryCacheRegion(uint8_t cls, MSRegionInfo* region, uint8_t slotLimit = kCachedRegionSlots);

    // Return a cached/active region back to the central pool using the same
    // classification as ReturnRegion.
    void ReturnRegionToCentral(MSRegionInfo* region, uint8_t cls);

    // Return current active Region to CentralRegionPool
    void ReturnRegion(uint8_t cls);

    // Per-size-class active Region (nullptr = no active Region for this class)
    MSRegionInfo* activeRegions_[kNumSizeClasses] = {};

    // O2/O9: Per-size-class cached backup Regions.
    // Written by gc-helper (BackgroundFillCache), consumed by mutator (SlowAlloc).
    // Base capacity stays at 2; a few hot classes may temporarily expand to 3 or 4.
    std::atomic<MSRegionInfo*> cachedRegion_[kNumSizeClasses][kCachedRegionSlots] = {};

    // O9: Per-size-class cache miss / heat heuristics.
    // cacheMiss_ tracks current refill pressure; cacheHeat_ keeps a short-lived
    // notion of hot classes so the background worker can prefill spare regions
    // for a few hot classes instead of sweeping all classes uniformly.
    std::atomic<uint8_t> cacheMiss_[kNumSizeClasses] = {};
    std::atomic<uint8_t> cacheHeat_[kNumSizeClasses] = {};
    std::atomic<uint8_t> hotHintCls_{kNoPreferredHotClass};
    std::atomic<uint8_t> cacheConsumed_[kNumSizeClasses] = {};

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
