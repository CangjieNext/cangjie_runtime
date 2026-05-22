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

#include "Heap/Collector/TracingCollector.h"
#include "Common/LFStack.h"

namespace MapleRuntime {

class CentralRegionPool;
class WriteBarrierBuffer;
struct MSRegionInfo;

// Per-GC-cycle timing and statistics for DFX logging (Step 5).
// Accumulated during a single GC cycle, logged in DumpMSGCLog(), then reset.
struct MSGCCycleStats {
    // ── Timing (microseconds) ──
    uint64_t preEnumRootsUs = 0;      // PreEnumRoots wall time: stop sweep, clear bitmaps, reclassify, prepare sweep.
    uint64_t markSetupUs = 0;         // EnumRoots wall time: handshake + EnumAndMarkRoots root discovery.
    uint64_t concurrentMarkUs = 0;    // ConcurrentMark wall time: RunConcurrentMarkLoop and assist snapshot.
    uint64_t markTerminationUs = 0;   // MarkTermination wall time: STW drain, WB flush, deferred queue drain.
    uint64_t postMarkUs = 0;          // PostMarkTermination wall time: weak refs, finalizers, large-object reclaim.
    uint64_t concurrentSweepUs = 0;   // ConcurrentSweep wall time: ComputeMarkedBytes and eager sweep sampling.
    uint64_t postGcHookUs = 0;        // PostGarbageCollection hook wall time after UpdateGCStats completes.

    // ── Counting ──
    size_t rootsCount = 0;                // markWorkStack_.size() right after EnumAndMarkRoots finishes.
    size_t markTermProcessedObjects = 0;  // Objects drained by ExecuteLikeDrainMainQueue during STW mark termination.
    size_t markTermNewlyMarkedObjects = 0; // markedObjectCount delta created by the STW final drain.
    size_t losFreedCount = 0;             // Large objects reclaimed in CollectLargeGarbage for this cycle.
    size_t losFreedRegionCount = 0;       // Large-object region spans reclaimed in CollectLargeGarbage.
    size_t losFreedBytes = 0;             // Bytes reclaimed from large-object spans in this cycle.
    uint64_t losCollectUs = 0;            // Timer around CollectLargeGarbage.

    // ── Previous cycle sweep summary (persisted from last cycle) ──
    size_t prevSweptSmallRegions = 0;         // Previous cycle TotalSweptRegions() snapshot used by LogSweepSummary.
    size_t prevSweptSmallEmptiedRegions = 0;  // Previous cycle emptiedRegions snapshot used by LogSweepSummary.
    size_t prevSweptLargeCount = 0;           // Previous cycle large-object reclaim count used by LogSweepSummary.
    size_t prevSweptLargeRegions = 0;         // Previous cycle large-object reclaimed region spans.
    size_t prevSweptLargeFreedBytes = 0;      // Previous cycle large-object reclaimed bytes.
    size_t prevBgSweepRegions = 0;            // Previous cycle background-swept small-region count.
    size_t prevLazySweepRegions = 0;          // Previous cycle lazy-swept small-region count.

    // ── Mutator assist stats (Step 2.2 §4) ──
    size_t assistCallCount = 0;          // assistCallTotal_ snapshot at ConcurrentMark end.
    size_t assistProcessedObjects = 0;   // assistProcessedCount_ snapshot at ConcurrentMark end.
    size_t assistFastSkipCount = 0;      // assistFastSkipCount_ snapshot for bg-credit short-circuit counts.
    size_t globalShareCount = 0;         // Number of WorkBuf pushes to globalFull_ from WB overflow paths.
    size_t bgScanCreditConsumed = 0;     // bgScanCreditConsumed_ snapshot after concurrent mark convergence.

    // ── True live bytes (computed from markBitmaps after marking) ──
    size_t trueLiveBytes = 0;            // ComputeMarkedBytes() result, used by UpdateGCStats and GC-end logs.

    // ── Eager sweep batch ──
    size_t eagerSweepBatchRegions = 0;   // Actual regions swept by EagerSweepBatch during ConcurrentSweep.
    size_t eagerSweepEmptiedRegions = 0; // Empty regions produced by eager sweep, derived from empty cache delta.
    size_t eagerSweepTotalUnswept = 0;   // Unswept region count sampled before eager sweep starts.

    // ── Region accounting snapshot ──
    size_t activeRegionCount = 0;        // activeRegionBytes / MS_REGION_SIZE, emitted in DumpRegionStatsMS.

    void Reset() { *this = MSGCCycleStats{}; }
};

// MarkSweepCollector — Mark-Sweep GC collector
// Inherits from TracingCollector, provides MS-GC framework.
// Step 2: Tri-color marking (PreEnumRoots → EnumRoots → ConcurrentMark → MarkTermination → PostMarkTermination)
class MarkSweepCollector : public TracingCollector {
public:
    class GreyWorkSink {
    public:
        virtual ~GreyWorkSink() = default;
        virtual void PushGrey(BaseObject* obj) = 0;
    };

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
    void PreGarbageCollection(bool isConcurrent) override;
    void PostGarbageCollection(uint64_t gcIndex) override;

    // ── MarkNewObject: keep new objects live and defer one scan until mark termination ──
    void MarkNewObject(BaseObject* obj) override;

    // ── Mutator assist interface (Step 2.2 §4) ──
    // Called from AllocBufferMS::Allocate() during concurrent marking.
    // Pays mutator assist debt (in object units) and returns paid objects.
    size_t MSMarkAssist(size_t debtObjects);
    uint64_t GetAssistWorkPerByteScaled() const
    {
        return assistWorkPerByteScaled_.load(std::memory_order_acquire);
    }
    static uint64_t GetAssistRatioScale();
    static double GetAssistRatioMaxMultiplier();
    size_t GetAssistDebtTriggerObjects() const;
    static MarkSweepCollector* GetActiveCollector()
    {
        return activeCollector_.load(std::memory_order_acquire);
    }
    // Flush an overflowed WriteBarrierBuffer to the global LFStack.
    void FlushWriteBarrierOverflow(WriteBarrierBuffer& wbBuf);

    // Per-P GCWork accessors for initialization during concurrent mark.
    LFStack& GetGlobalFull() { return globalFull_; }
    WorkBufPool& GetWorkBufPool() { return workBufPool_; }

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
    void MergeRootsFromAllocBuffers(WorkStack& workStack) override;

    bool ShouldIgnoreRequest(GCRequest& request) override { return request.ShouldBeIgnored(); }
    void UpdateGCStats() override;
    bool TryEnqueueGreyObject(BaseObject* obj, GreyWorkSink& sink);

private:
    // ── MS GC phases ──
    void DoGarbageCollection(uint64_t gcIndex);
    void PreEnumRoots();        // Step 2.1a: Stop sweep, clear bitmaps, reclassify
    void EnumRoots();            // Step 2.1b: Handshake + root enumeration
    void ConcurrentMark();      // Step 2.2: Concurrent marking (GC thread + assist)
    void MarkTermination();     // Step 2.3: STW finalization of marking
    void PostMarkTermination(); // Step 2.4: Non-STW cleanup (WeakRef, Finalizer, large reclaim)
    void ConcurrentSweep();     // Step 3 (stub)

    // ── Step 2.1 internals ──
    void ClearAllMarkBitmaps();
    void EnsureSweepDone();
    void EnumAndMarkRoots(WorkStack& workStack);
    void DiscoverAndPushObject(BaseObject* obj, WorkStack& workStack);
    bool TryEnqueueGreyObject(BaseObject* obj, GreyWorkQueue& queue);
    bool DiscoverObject(BaseObject* obj);
    void DebugVerifyQueuedGreyObject(const BaseObject* obj, const char* queueName) const;

    // ── Step 2.2 internals ──
    void RunConcurrentMarkLoop();
    // Returns true when true convergence is reached and the loop should break.
    // Manages convergenceStableRounds_ and flushes localCreditAccum on convergence.
    bool CheckConvergenceAndBreak(size_t& localCreditAccum);
    void DrainMarkWorkStack(size_t maxObjects, size_t& scannedCount);
    void DrainConcurrentOverflowQueueToMarkWork();
    void DrainGCWork(PerPGCWork& gcw, size_t maxObjects, size_t& scannedCount);
    void TraceObjectRefFieldsToGCWork(BaseObject* obj, PerPGCWork& gcw);
    void TraceRefField(BaseObject* obj, RefField<>& field, WorkStack& workStack);
    void TransferRootsToGCWork();

    // ── Step5: LFStack global queue helpers ──
    void PushSingleToDeferredMainThread(BaseObject* obj);
    void DrainGlobalLFStackToMainQueue(GreyWorkQueue& mainQueue);
    size_t TryStealBgCreditPartial(size_t debtObjects);
    void InitAllPerPGCWork();
    void DisposeAllPerPGCWork();
    void ResetAllPerPGCWork();

    // ── Step 2.3 internals ──
    void CollectUnmarkedToMainQueue(GreyWorkQueue& mainQueue);
    void FlushAllWriteBarrierBuffers(GreyWorkQueue& mainQueue);

    void DrainDeferredMainThreadQueueToMainQueue(GreyWorkQueue& mainQueue);
    void ProcessDeferredWeakRefsPreSTW();  // Non-STW WeakRef processing after concurrent mark
    void CollectExternCandidatesToMain(GreyWorkQueue& mainQueue);
    void CollectResurrectionCandidatesToMain(GreyWorkQueue& mainQueue);
    void ExecuteLikeDrainMainQueue(GreyWorkQueue& mainQueue);

    // ── Step 2.4 internals ──
    void PrepareCycleRef();
    void PostResolveCycleTask();
    void CollectLargeGarbage();

    // ============================================================
    // Assist tuning parameters
    // ============================================================
    //
    // Fixed constants (compile-time):
    //   kCreditFlushThreshold      - GC thread accumulates bgScanCredit locally
    //                                and flushes every N objects to reduce atomic traffic.
    //   kSliceMaxObjects           - GC thread drain slice size per loop iteration.
    //   kConvergenceRequiredRounds - Consecutive empty rounds before triggering
    //                                the per-P flush + convergence check.
    //   kConvergenceYieldUs        - usleep duration when stable rounds < threshold.
    //   kConvergenceFlushWaitUs    - usleep before per-P flush to let in-flight assists finish.
    //
    // Environment variables (runtime):
    //   CJ_MS_ASSIST_RATIO_COEFF        - Escalation coefficient per loop iteration.
    //                                     ratio *= (1 + coeff) each iteration.
    //                                     Range: (0, 10], default 0.1.
    //   CJ_MS_ASSIST_RATIO_SCALE        - Fixed-point scale denominator (positive integer).
    //                                     Default 1024.
    //   CJ_MS_ASSIST_DEBT_TRIGGER       - Objects per assist call trigger threshold.
    //                                     Default 256.
    //   CJ_MS_ASSIST_RATIO_MAX_MULT     - Maximum ratio escalation multiplier over the initial
    //                                     value. Controls how much assist participates in long
    //                                     GC cycles. Range: (0, 100], default 10.
    //                                     Example: 1.5 means ratio can grow at most 1.5x the
    //                                     initial value, yielding moderate assist for long GC
    //                                     cycles while keeping short GC cycles assist-free.
    // ============================================================
    static constexpr size_t   kCreditFlushThreshold     = 1024;
    static constexpr size_t   kSliceMaxObjects          = 256;
    static constexpr size_t   kConvergenceRequiredRounds     = 3;
    static constexpr uint64_t kConvergenceYieldUs            = 50;
    static constexpr uint64_t kConvergenceFlushWaitUs        = 200;

    // ── Convergence tracking ──
    size_t convergenceStableRounds_ = 0;

    // ── Work stack (populated in EnumRoots, consumed in ConcurrentMark) ──
    GreyWorkQueue markWorkStack_;
    GreyWorkQueue concurrentOverflowQueue_;
    mutable std::mutex concurrentOverflowQueueMutex_;

    // ── Step5: LFStack global work queue ──
    LFStack globalFull_;
    WorkBufPool workBufPool_;
    PerPGCWork gcThreadWork_;
    std::atomic<int64_t> bgScanCredit_{0};
    std::atomic<uint64_t> assistWorkPerByteScaled_{0};
    std::atomic<size_t> prevCycleMarkedObjects_{0};

    // ── Main-thread-only deferred work (e.g. weak refs discovered by assist) ──
    GreyWorkQueue deferredMainThreadQueue_;
    mutable std::mutex deferredMainThreadMutex_;

    // ── Assist control ──
    static std::atomic<MarkSweepCollector*> activeCollector_;

    // (assist parameters are grouped above with other tuning constants)

    // ── Assist concurrent stats ──
    std::atomic<size_t> assistCallTotal_{0};
    std::atomic<size_t> assistProcessedCount_{0};
    std::atomic<size_t> assistFastSkipCount_{0};
    std::atomic<size_t> globalShareCount_{0};
    std::atomic<size_t> bgScanCreditConsumed_{0};

    // ── DFX: Per-cycle stats ──
    MSGCCycleStats cycleStats_;
    uint64_t gcCycle_ = 0;  // Monotonic GC cycle counter
    uint16_t currentTagID_ = 0;  // ForwardDataManager tag alternation

    // ── DFX: Logging methods (Step 5) ──
    void LogGCEntry(uint64_t gcIndex, GCReason reason);           // Block ①
    void DumpRegionStatsMS();                                     // Block ⑤
    void LogGCSummary(uint64_t gcTimeUs);                         // Block ⑥
    void SnapshotPreviousCycleSweepStats();                       // Persist previous sweep stats
    void LogSweepSummary();                                       // Block ④
    void DumpGCStateSnapshot(const char* stage);

    size_t prevCycleLargeFreedCount_ = 0;
    size_t prevCycleLargeFreedRegionCount_ = 0;
    size_t prevCycleLargeFreedBytes_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MARK_SWEEP_COLLECTOR_H
