// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_SWEEP_COLLECTOR_H
#define MRT_MARK_SWEEP_COLLECTOR_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <mutex>
#include <vector>

#include "TracingCollector.h"

namespace MapleRuntime {

class CentralRegionPool;
class LargeObjectSpace;
class WriteBarrierBuffer;
struct MSRegionInfo;

// Per-GC-cycle timing and statistics for DFX logging (Step 5).
// Accumulated during a single GC cycle, logged in DumpMSGCLog(), then reset.
struct MSGCCycleStats {
    // ── Timing (microseconds) ──
    uint64_t markSetupUs = 0;
    uint64_t clearBitmapUs = 0;
    uint64_t forceFinishSweepUs = 0;
    uint64_t enumRootsUs = 0;
    uint64_t concurrentMarkUs = 0;
    uint64_t markTerminationUs = 0;
    uint64_t postMarkUs = 0;

    // ── Counting ──
    size_t clearBitmapRegions = 0;
    size_t forceFinishSweptRegions = 0;
    size_t rootsCount = 0;
    size_t concMarkThreads = 0;
    size_t wbBufFlushCount = 0;
    size_t wbBufEntriesTotal = 0;
    uint64_t wbBufFlushUs = 0;
    size_t drainedGreyObjects = 0;
    uint64_t drainMarkQueueUs = 0;
    size_t weakRefCleared = 0;
    size_t finalizerEnqueued = 0;
    size_t losFreedCount = 0;
    size_t losFreedBytes = 0;
    uint64_t losCollectUs = 0;
    size_t preparedRegions = 0;
    uint64_t prepareForSweepUs = 0;

    // ── Previous cycle sweep summary (persisted from last cycle) ──
    size_t prevSweptSmallRegions = 0;
    size_t prevSweptSmallFreedBytes = 0;
    size_t prevSweptLargeCount = 0;
    size_t prevSweptLargeFreedBytes = 0;
    size_t prevReleasedRegions = 0;
    size_t prevBgSweepRegions = 0;
    size_t prevLazySweepRegions = 0;
    size_t prevForceSweepRegions = 0;

    // ── Mutator assist stats (Step 2.2 §4) ──
    size_t assistCallCount = 0;
    size_t assistMarkedObjects = 0;

    // ── True live bytes (computed from markBitmaps after marking) ──
    size_t trueLiveBytes = 0;

    void Reset() { *this = MSGCCycleStats{}; }
};

// MarkSweepCollector — Mark-Sweep GC collector
// Inherits from TracingCollector, provides MS-GC framework.
// Step 2: Tri-color marking (MarkSetup → ConcurrentMark → MarkTermination → PostMarkTermination)
class MarkSweepCollector : public TracingCollector {
public:
    explicit MarkSweepCollector(Allocator& allocator, CollectorResources& resources)
        : TracingCollector(allocator, resources)
    {
        collectorType = CollectorType::MARK_SWEEP_COLLECTOR;
    }

    ~MarkSweepCollector() override = default;

    // ── Lifecycle ──
    void Init() override;
    void Fini() override;

    // ── Core GC entry ──
    void RunGarbageCollection(uint64_t gcIndex, GCReason reason) override;

    // ── MarkNewObject: mark newly allocated objects black during marking phases ──
    void MarkNewObject(BaseObject* obj) override;

    // ── Mutator assist interface (Step 2.2 §4) ──
    // Called from AllocBufferMS::Allocate() during concurrent marking.
    // Drains grey objects from global queue in a budget-limited batch.
    void MSMarkAssist();
    static MarkSweepCollector* GetActiveCollector()
    {
        return activeCollector_.load(std::memory_order_acquire);
    }

    // Called from MSWriteBarrier when WriteBarrierBuffer overflows.
    // Pushes a single grey object to the global work queue for tracing.
    void EnqueueGreyObject(BaseObject* obj);

    // Flush an overflowed WriteBarrierBuffer to the global work queue.
    // Takes one mutex lock for the entire batch. Clears the buffer afterwards.
    void FlushWriteBarrierOverflow(WriteBarrierBuffer& wbBuf);

    // ── CopyGC-specific overrides: safe no-ops ──
    BaseObject* ForwardObject(BaseObject*) override { return nullptr; }
    bool IsFromObject(BaseObject*) const override { return false; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }
    BaseObject* FindToVersion(BaseObject*) const override { return nullptr; }

    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool TryForwardRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool TryUntagRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool TryTagRefField(BaseObject*, RefField<>&, BaseObject*) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override;

    bool IsOldPointer(RefField<>&) const override { return false; }
    bool IsCurrentPointer(RefField<>&) const override { return true; }

    // ── MS-specific MarkObject: write to MSRegionInfo::markBitmap, not CopyGC LiveInfo ──
    bool MarkObject(BaseObject* obj) const override;

    void FlipTagID() { currentTagID_ ^= 1; }
    uint16_t GetCurrentTagID() override { return currentTagID_; }

    void AddRawPointerObject(BaseObject*) override
    {
        // Mark-Sweep doesn't move objects — no region pinning needed.
    }
    void RemoveRawPointerObject(BaseObject*) override
    {
        // Mark-Sweep doesn't move objects — no region pinning needed.
    }
    void ResolveCycleRef() override {}

    // ── MS-specific overrides (TracingCollector virtuals) ──
    void EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const override;
    void EnumAndTagRawRoot(ObjectRef& root, RootSet& rootSet) const override;
    void TraceObjectRefFields(BaseObject* obj, WorkStack& workStack) override;
    BaseObject* GetAndTryTagObj(BaseObject* obj, RefField<>& field) override;
    void ProcessFinalizers() override;

    bool ShouldIgnoreRequest(GCRequest& request) override { return request.ShouldBeIgnored(); }
    void UpdateGCStats() override;

private:
    // ── MS GC phases ──
    void DoGarbageCollection(uint64_t gcIndex);
    void MarkSetup();           // Step 2.1: Handshake, bitmap clear, root enum
    void ConcurrentMark();      // Step 2.2: Concurrent marking (GC thread + assist)
    void MarkTermination();     // Step 2.3: STW finalization of marking
    void PostMarkTermination(); // Step 2.4: Non-STW cleanup (WeakRef, SATB, Finalizer, LOS)
    void ConcurrentSweep();     // Step 3 (stub)

    // ── Step 2.1 internals ──
    void ClearAllMarkBitmaps();
    void EnsureSweepDone();
    void EnumAndMarkRoots(WorkStack& workStack, WorkStack& foreignStack);

    // ── Step 2.2 internals ──
    void RunConcurrentMarkLoop(WorkStack& workStack);
    void RunCollectorDrainSlice(WorkStack& workStack, size_t maxObjects);
    void TraceRefField(BaseObject* obj, RefField<>& field, WorkStack& workStack);

    // ── Step 2.2 global queue helpers (GC thread + Mutator assist) ──
    void PushToGlobal(WorkStack& source);
    void PushSingleToGlobal(BaseObject* obj);
    void PopFromGlobal(WorkStack& dest, size_t maxBufs);
    void ShareWorkToGlobal(WorkStack& local);

    // ── Step 2.3 internals ──
    void CollectUnmarkedToMainQueue(WorkStack& mainQueue);
    void FlushAllWriteBarrierBuffers(WorkStack& mainQueue);
    void ClearAllWriteBarrierBuffers();
    void CollectExternCandidatesToMain(WorkStack& mainQueue);
    void CollectResurrectionCandidatesToMain(WorkStack& mainQueue);
    void ExecuteLikeDrainMainQueue(WorkStack& mainQueue);

    // ── Step 2.4 internals ──
    void PrepareCycleRef();
    void PostResolveCycleTask();
    void CollectLargeGarbage();

    // ── Convergence tracking ──
    size_t convergenceStableRounds_ = 0;
    static constexpr size_t kConvergenceRequiredRounds = 3;

    // ── Inter-phase work stacks (populated in MarkSetup, consumed in ConcurrentMark) ──
    WorkStack markWorkStack_;
    WorkStack markForeignStack_;

    // ── Drain slice parameters ──
    static constexpr size_t kSliceMaxObjects = 256;

    // ── Global work queue for concurrent marking (GC thread + Mutator assist) ──
    WorkStack globalWorkQueue_;
    mutable std::mutex globalWorkMutex_;

    // ── Assist control ──
    std::atomic<bool> assistEnabled_{false};
    static std::atomic<MarkSweepCollector*> activeCollector_;

    // ── Assist parameters (MVP: fixed values, per Step 2.2 §4.6) ──
    //    size() returns buffer count (each buf holds ≤64 objects)
    static constexpr size_t kAssistMaxObjectsPerCall = 64;     // max objects per assist call
    static constexpr size_t kAssistLocalFlushBufs = 2;         // flush local→global when > 2 bufs
    static constexpr size_t kShareWorkBufs = 2;                // GC thread shares when local > 2 bufs

    // ── Assist concurrent stats ──
    std::atomic<size_t> assistMarkedCount_{0};
    std::atomic<size_t> assistCallTotal_{0};

    // ── DFX: Per-cycle stats ──
    MSGCCycleStats cycleStats_;
    uint64_t gcCycle_ = 0;  // Monotonic GC cycle counter
    uint16_t currentTagID_ = 0;  // ForwardDataManager tag alternation

    // ── DFX: Logging methods (Step 5) ──
    void LogGCEntry(uint64_t gcIndex, GCReason reason);           // Block ①
    void LogSweepSummary();                                       // Block ④
    void DumpRegionStatsMS();                                     // Block ⑤
    void DumpFragmentationStatsMS();                              // Block ⑤-b
    void LogGCSummary(uint64_t gcTimeUs);                         // Block ⑥
    void SnapshotPreviousCycleSweepStats();                       // Persist previous sweep stats

    size_t prevCycleLargeFreedCount_ = 0;
    size_t prevCycleLargeFreedBytes_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MARK_SWEEP_COLLECTOR_H
