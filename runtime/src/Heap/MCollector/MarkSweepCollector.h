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
    uint64_t preEnumRootsUs = 0;
    uint64_t markSetupUs = 0;
    uint64_t clearBitmapUs = 0;
    uint64_t enumRootsUs = 0;
    uint64_t concurrentMarkUs = 0;
    uint64_t markTerminationUs = 0;
    uint64_t postMarkUs = 0;
    uint64_t concurrentSweepUs = 0;
    uint64_t postGcHookUs = 0;

    // ── Counting ──
    size_t clearBitmapRegions = 0;
    size_t forceFinishSweptRegions = 0;
    size_t rootsCount = 0;
    size_t concMarkThreads = 0;
    size_t wbBufFlushCount = 0;
    size_t wbBufEntriesTotal = 0;
    uint64_t wbBufFlushUs = 0;
    size_t markTermProcessedObjects = 0;
    size_t markTermNewlyMarkedObjects = 0;
    uint64_t drainMarkQueueUs = 0;
    size_t weakRefCleared = 0;
    size_t finalizerEnqueued = 0;
    size_t losFreedCount = 0;
    size_t losFreedRegionCount = 0;
    size_t losFreedBytes = 0;
    uint64_t losCollectUs = 0;
    size_t preparedRegions = 0;
    uint64_t prepareForSweepUs = 0;

    // ── Per-phase mark deltas (for DFX per-phase logging) ──
    size_t markAfterEnumRoots = 0;     // markedObjectCount after EnumRoots
    size_t markAfterConcMark = 0;      // markedObjectCount after ConcurrentMark
    size_t markAfterMarkTerm = 0;      // markedObjectCount after MarkTermination

    // ── Previous cycle sweep summary (persisted from last cycle) ──
    size_t prevSweptSmallRegions = 0;
    size_t prevSweptSmallEmptiedRegions = 0;
    size_t prevSweptSmallRetainedRegions = 0;
    size_t prevSweptSmallFreedBytes = 0;
    size_t prevSweptLargeCount = 0;
    size_t prevSweptLargeRegions = 0;
    size_t prevSweptLargeFreedBytes = 0;
    size_t prevEmptiedRegions = 0;
    size_t prevBgSweepRegions = 0;
    size_t prevLazySweepRegions = 0;
    size_t prevReleasedTreeRegions = 0;
    size_t prevReleasedTreeBytes = 0;

    // ── Mutator assist stats (Step 2.2 §4) ──
    size_t assistCallCount = 0;
    size_t assistMarkedObjects = 0;
    size_t assistProcessedObjects = 0;
    size_t assistTracedObjects = 0;
    size_t assistFastSkipCount = 0;
    size_t globalShareCount = 0;
    size_t globalSharedBufs = 0;
    size_t globalPopHitCount = 0;
    size_t globalPopMissCount = 0;
    size_t bgScanCreditConsumed = 0;
    size_t lfStackPushCount = 0;
    size_t lfStackPopCount = 0;

    // ── True live bytes (computed from markBitmaps after marking) ──
    size_t trueLiveBytes = 0;

    // ── Eager sweep batch (方案3) ──
    size_t eagerSweepBatchRegions = 0;

    // ── Region accounting snapshot ──
    size_t activeRegionCount = 0;
    size_t releasedTreeRegionCount = 0;
    size_t virginRegionCount = 0;
    size_t committedRegionCount = 0;

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
    static constexpr uint64_t GetAssistRatioScale() { return kAssistRatioScale; }
    size_t GetAssistDebtTriggerObjects() const { return kAssistDebtTriggerObjects; }
    static MarkSweepCollector* GetActiveCollector()
    {
        return activeCollector_.load(std::memory_order_acquire);
    }
    bool HasPendingGlobalAssistWork() const
    {
        return !globalFull_.Empty();
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

    // ── Step 2.2 internals (Step5: PerPGCWork + LFStack) ──
    void RunConcurrentMarkLoop();
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
    void CollectExternCandidatesToMain(GreyWorkQueue& mainQueue);
    void CollectResurrectionCandidatesToMain(GreyWorkQueue& mainQueue);
    void ExecuteLikeDrainMainQueue(GreyWorkQueue& mainQueue);

    // ── Step 2.4 internals ──
    void PrepareCycleRef();
    void PostResolveCycleTask();
    void CollectLargeGarbage();

    // ── Convergence tracking ──
    size_t convergenceStableRounds_ = 0;
    static constexpr size_t kConvergenceRequiredRounds = 3;

    // ── Work stack (populated in EnumRoots, consumed in ConcurrentMark) ──
    GreyWorkQueue markWorkStack_;

    // ── Drain slice parameters ──
    static constexpr size_t kSliceMaxObjects = 256;

    // ── Opt2: bgScanCredit batch flush threshold (in objects) ──
    static constexpr size_t kCreditFlushThreshold = 1024;
    static constexpr uint64_t kAssistRatioScale = 1024;

    // ── Step5: LFStack global work queue ──
    LFStack globalFull_;
    WorkBufPool workBufPool_;
    PerPGCWork gcThreadWork_;
    std::atomic<int64_t> bgScanCredit_{0};
    std::atomic<uint64_t> assistWorkPerByteScaled_{0};
    std::atomic<size_t> prevCycleMarkedObjects_{0};
    std::atomic<int64_t> scanWorkRemaining_{0};
    size_t assistThresholdBytes_ = 0;
    size_t assistLiveBaselineBytes_ = 0;
    size_t assistAllocBaselineBytes_ = 0;

    // ── Main-thread-only deferred work (e.g. weak refs discovered by assist) ──
    GreyWorkQueue deferredMainThreadQueue_;
    mutable std::mutex deferredMainThreadMutex_;

    // ── Assist control ──
    std::atomic<bool> assistEnabled_{false};
    static std::atomic<MarkSweepCollector*> activeCollector_;

    // ── Assist parameters (Step5: simplified with Per-P + bgCredit) ──
    static constexpr size_t kAssistMaxObjectsPerCall = 512;  // max objects per assist call
    static constexpr size_t kAssistDebtTriggerObjects = 64;

    // ── Assist concurrent stats ──
    std::atomic<size_t> assistMarkedCount_{0};
    std::atomic<size_t> assistCallTotal_{0};
    std::atomic<size_t> assistProcessedCount_{0};
    std::atomic<size_t> assistTracedCount_{0};
    std::atomic<size_t> assistFastSkipCount_{0};
    std::atomic<size_t> globalShareCount_{0};
    std::atomic<size_t> globalSharedBufCount_{0};
    std::atomic<size_t> globalPopHitCount_{0};
    std::atomic<size_t> globalPopMissCount_{0};
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

    size_t prevCycleLargeFreedCount_ = 0;
    size_t prevCycleLargeFreedRegionCount_ = 0;
    size_t prevCycleLargeFreedBytes_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MARK_SWEEP_COLLECTOR_H
