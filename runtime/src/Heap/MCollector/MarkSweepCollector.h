// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_SWEEP_COLLECTOR_H
#define MRT_MARK_SWEEP_COLLECTOR_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "Heap/Collector/TracingCollector.h"
#include "Common/LFStack.h"
#include "Heap/MCollector/MSAssistPacing.h"
#include "Heap/MCollector/MSHeapThresholdPolicy.h"
#include "Heap/MCollector/MSMarkLocalStats.h"
#include "Heap/MCollector/MSMarkTerminationPolicy.h"
#include "Heap/MCollector/MSMarkWorkerControl.h"
#include "Heap/MCollector/MSMarkWorkerPolicy.h"

namespace MapleRuntime {

class CentralRegionPool;
class AllocBufferMS;
struct MSRegionInfo;
struct WorkBuf;

// Per-GC-cycle timing and statistics for DFX logging (Step 5).
// Accumulated during a single GC cycle, logged in LogGCSummary(), then reset.
struct MSGCCycleStats {
    // ── Timing (microseconds) ──
    uint64_t preEnumRootsUs = 0;      // PreEnumRoots wall time: stop sweep, clear bitmaps, reclassify unswept.
    uint64_t markSetupUs = 0;         // EnumRoots wall time: handshake + EnumAndMarkRoots root discovery.
    uint64_t concurrentMarkUs = 0;    // ConcurrentMark wall time: RunConcurrentMarkLoop and assist snapshot.
    uint64_t markTerminationUs = 0;   // MarkTermination wall time: STW drain, WB flush, deferred queue drain, idle transition.
    uint64_t postMarkUs = 0;          // PostMarkTermination wall time: post-mark cleanup, stats and Sweep/Scavenge handoff.
    uint64_t postGcHookUs = 0;        // PostGarbageCollection hook wall time after UpdateGCStats completes.

    // ── Counting ──
    size_t rootsCount = 0;                // markWorkStack_.size() right after EnumAndMarkRoots finishes.
    size_t gcMainProcessedObjects = 0;    // Concurrent buffers scanned by the GC main thread.
    size_t gcMainProcessedBuffers = 0;    // WorkBufs completed by the GC main thread.
    size_t helperProcessedObjects = 0;    // Concurrent buffers scanned by all helper workers.
    size_t markCheckpointCount = 0;
    size_t markDebtBuffers = 0;
    size_t markPressureLevel = 0;
    size_t markNoProgressWindows = 0;
    MSMarkTerminationReason markTerminationReason = MSMarkTerminationReason::NATURAL;

    // ── Mutator assist stats (Step 2.2 §4) ──
    size_t assistProcessedObjects = 0;

    // ── True live bytes (computed from markBitmaps after marking) ──
    size_t trueLiveBytes = 0;            // ComputeMarkedStats() result, used by UpdateGCStats and GC-end logs.

    // ── Floating-live threshold baseline ──
    size_t markStartInUseBytes = 0;
    size_t markEndInUseBytes = 0;
    size_t inUseBurstBytes = 0;
    size_t thresholdBaseBytes = 0;
    bool floatingLiveInflated = false;

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
    size_t MSMarkAssist(AllocBufferMS& allocBuffer, size_t debtObjects);
    uint64_t GetAssistWorkPerByteScaled() const
    {
        return assistWorkPerByteScaled_.load(std::memory_order_acquire);
    }
    static constexpr uint64_t GetAssistRatioScale() { return kMSAssistRatioScale; }
    static MarkSweepCollector* GetActiveCollector()
    {
        return activeCollector_.load(std::memory_order_acquire);
    }
    // Write-barrier log lifecycle.
    // Ensure the owner has an activeLog_ WorkBuf to append into; returns it
    // (nullptr only on pool exhaustion).
    WorkBuf* EnsureActiveLog(AllocBufferMS& ab);
    // Overflow: publish the full activeLog_ wholesale into globalFull_, then
    // install a fresh empty log. Entries have already won the mark CAS.
    void PublishAndSwapLog(AllocBufferMS& ab);
    // Owner exit: publish the partial activeLog_ to globalFull_, and return all
    // cached WorkBufs to the pool.
    void PublishExitedLog(AllocBufferMS& ab);

    // Route-A (Step37 §10): shade (mark) an overwritten old-ref at write time.
    // Returns true if THIS call won the mark-CAS (object was white → now grey),
    // meaning the caller should log it for later scanning; false if it was
    // already grey/black or not a traceable object (perfect dedup at the source).
    bool ShadeRefForBarrier(BaseObject* ref, TypeInfo** markedTypeInfo = nullptr)
    {
        return DiscoverObject(ref, markedTypeInfo);
    }
    // Same operation for barrier paths that already rejected non-heap refs.
    // This is Runtime-internal and only skips the duplicate heap-range check;
    // Region/slot/stateWord validation remains mandatory.
    bool ShadeManagedRefForBarrier(BaseObject* ref, TypeInfo** markedTypeInfo = nullptr)
    {
        return DiscoverManagedObject(ref, markedTypeInfo);
    }
    // Resolves a known-managed small object to its mark-bitmap word and bit.
    // LARGE objects return false and retain the dedicated managed shade path.
    bool ResolveManagedSmallRefForBarrier(BaseObject* ref, MSRegionInfo*& info,
                                          size_t& wordIdx, uint64_t& bitMask,
                                          TypeInfo*& whiteTypeInfo) const;

    // ── CopyGC-specific overrides: safe no-ops ──
    BaseObject* ForwardObject(BaseObject*) override { return nullptr; }
    bool IsFromObject(BaseObject*) const override { return false; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }
    BaseObject* FindToVersion(BaseObject*) const override { return nullptr; }

    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool TryForwardRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool TryUntagRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override;

    bool IsOldPointer(RefField<>&) const override { return false; }
    bool IsCurrentPointer(RefField<>&) const override { return true; }

    // ── MS-specific MarkObject: write to MSRegionInfo::markBitmap, not CopyGC LiveInfo ──
    bool MarkObject(BaseObject* obj) const override;

    uint16_t GetCurrentTagID() override { return 0; }

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
    void DoGarbageCollection();
    void PreEnumRoots();        // Step 2.1a: Stop sweep, clear bitmaps, reclassify
    void EnumRoots();            // Step 2.1b: Handshake + root enumeration
    void ConcurrentMark();      // Step 2.2: Concurrent marking (GC thread + assist)
    void MarkTermination();     // Step 2.3: STW finalization of marking
    void PostMarkTermination(); // Step 2.4: Non-STW cleanup, stats and Sweep/Scavenge handoff

    // ── Step 2.1 internals ──
    void EnumAndMarkRoots(WorkStack& workStack);
    void DiscoverAndPushObject(BaseObject* obj, WorkStack& workStack);
    bool TryEnqueueGreyObject(BaseObject* obj, GreyWorkQueue& queue);
    bool DiscoverObject(BaseObject* obj, TypeInfo** markedTypeInfo = nullptr);
    bool DiscoverManagedObject(BaseObject* obj, TypeInfo** markedTypeInfo = nullptr);
    // Bitmap-only discovery. MSGC derives cycle object counts from a post-mark
    // bitmap walk instead of updating a hot global counter.
    // Diagnostic: after marking completes (in PostMarkTermination, before sweep
    // reclaims anything), walk all marked objects and report any ref edge that
    // points to an unmarked target. Such a target will be wrongly swept while
    // still reachable from a live object — the classic missed-mark UAF.
    // Gated by env cjMSVerifyMark; logs the first N violations only.
    void VerifyMarkCompleteness();

    // ── Step 2.2 internals ──
    void RunConcurrentMarkLoop();
    void DrainMarkWorkStack();
    // Claim and drain one published buffer. GC main/helpers accept any length;
    // mutator assist passes requireFull=true to preserve its credit granularity.
    size_t DrainOneBufRound(PerPGCWork& gcw, bool requireFull,
                            LFStack::Counts* countsAfterPop = nullptr,
                            bool* processedBuffer = nullptr);
    size_t DrainCurrentBuf(PerPGCWork& gcw);
    void TraceObjectRefFieldsToGCWork(BaseObject* obj, TypeInfo* typeInfo,
                                      PerPGCWork& gcw);
    void TraceRefField(RefField<>& field, WorkStack& workStack);
    void TransferRootsToGCWork();
    void StartMarkWorkers(size_t maxHelpers, size_t initialHelpers);
    void RunMarkWorker(size_t workerId);
    size_t QuiesceMarkWorkers();
    void SnapshotAssistStats();
    void MergeExitedAssistStats(AllocBufferMS& allocBuffer);

    // ── Step5: LFStack global queue helpers ──
    void PushSingleToDeferredMainThread(BaseObject* obj);
    void PushTrustedGreyToDeferredMainThread(BaseObject* obj);
    void DrainGlobalLFStackToMainQueue(GreyWorkQueue& mainQueue);
    size_t TryStealBgCreditPartial(size_t debtObjects);
    void InitAllPerPGCWork();
    void DisposeAllPerPGCWork();
    void ResetAllPerPGCWork();

    // ── Step 2.3 internals ──
    void FlushAllWriteBarrierBuffers(GreyWorkQueue& mainQueue);

    void DrainDeferredMainThreadQueueToMainQueue(GreyWorkQueue& mainQueue);
    size_t DrainMarkTerminationToFixedPoint(GreyWorkQueue& mainQueue);
    void PrepareFinalizersForResurrection();
    void CollectResurrectionCandidatesToMain(GreyWorkQueue& mainQueue);
    void ExecuteLikeDrainMainQueue(GreyWorkQueue& mainQueue);
    void ClearWeakRefsAfterMark();

    // ── Step 2.4 internals ──
    void PrepareCycleRef();
    void PostResolveCycleTask();
    void CollectLargeGarbage();
    void RunOOMEmergencyParallelSweep(CentralRegionPool& centralPool);

    // ============================================================
    // Assist tuning parameters
    // ============================================================
    //
    // Environment variables (runtime):
    //   cjMSAssistBaseRatioPercent      - Percentage applied to the computed base ratio. Default 50.
    //   cjMSAssistBacklogStepBuffers    - Full backlog buffers per 1x increase. Default 16.
    //   cjMSAssistMaxMultiplier         - Maximum backlog multiplier. Default 4.
    //   cjMSAssistCreditDecayRounds     - Background-credit linear decay rounds. Default 100.
    // ============================================================
    // ── Work stack (populated in EnumRoots, consumed in ConcurrentMark) ──
    GreyWorkQueue markWorkStack_;

    // ── Step5: LFStack global work queue ──
    WorkEpoch markWorkEpoch_;
    LFStack globalFull_{&markWorkEpoch_};
    WorkBufPool workBufPool_;
    PerPGCWork gcThreadWork_;
    struct MSMarkWorkerSlot {
        PerPGCWork work;
        MSMarkWorkerLocalStats stats;
    };
    std::vector<std::unique_ptr<MSMarkWorkerSlot>> markWorkerSlots_;
    MSMarkWorkerControl markWorkerControl_{&markWorkEpoch_};
    std::atomic<int64_t> bgScanCredit_{0};
    std::atomic<uint64_t> assistWorkPerByteScaled_{0};
    std::atomic<size_t> prevCycleMarkedObjects_{0};
    uint64_t previousAssistBaseRatioScaled_ = 1;
    size_t previousAssistProcessedObjects_ = 0;
    size_t previousBackgroundProcessedObjects_ = 0;
    bool assistBaseRatioInitialized_ = false;

    // ── Main-thread-only deferred work (e.g. weak refs discovered by assist) ──
    GreyWorkQueue deferredMainThreadQueue_;
    mutable std::mutex deferredMainThreadMutex_;

    // ── Assist control ──
    static std::atomic<MarkSweepCollector*> activeCollector_;

    // (assist parameters are grouped above with other tuning constants)

    // Mutators that exit during mark fold their processed-work feedback here.
    // Active mutators are aggregated directly under STW at mark termination.
    std::mutex exitedAssistStatsMutex_;
    MSMarkAssistStats exitedAssistStats_;

    // ── DFX: Per-cycle stats ──
    MSGCCycleStats cycleStats_;
    MSScavengeThresholdState scavengeThresholdState_;
    size_t previousReusedScavengedRegionCount_ = 0;
    uint64_t gcCycle_ = 0;  // Monotonic GC cycle counter

    // ── DFX: Logging methods (Step 5) ──
    void LogGCEntry(uint64_t gcIndex, GCReason reason);           // Block ①
    void DumpRegionStatsMS();                                     // Block ⑤
    void LogGCSummary(uint64_t gcTimeUs);                         // Block ⑥
    void DumpGCStateSnapshot(const char* stage);
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MARK_SWEEP_COLLECTOR_H
