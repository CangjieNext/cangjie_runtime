// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_SWEEP_COLLECTOR_H
#define MRT_MARK_SWEEP_COLLECTOR_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <vector>

#include "Heap/Collector/TracingCollector.h"
#include "Heap/MCollector/MSAssistPacing.h"
#include "Heap/MCollector/MSHeapThresholdPolicy.h"
#include "Heap/MCollector/MSMarkBatch.h"
#include "Heap/MCollector/MSRootDedupTable.h"
#include "Heap/MCollector/MSMarkScheduler.h"
#include "Heap/MCollector/MSMarkTerminationPolicy.h"
#include "Heap/MCollector/MSMarkWorkerControl.h"
#include "Heap/MCollector/MSMarkWorkerPolicy.h"
#include "Heap/MCollector/MSWorkUnit.h"

namespace MapleRuntime {

class CentralRegionPool;
class AllocBufferMS;
struct MSBarrierMarkBuffer;
struct MSRegionInfo;
struct ResolvedMarkTarget;

// Per-GC-cycle timing and statistics for DFX logging (Step 5).
// Accumulated during a single GC cycle, logged in LogGCSummary(), then reset.
struct MSGCCycleStats {
    // ── Timing (microseconds) ──
    uint64_t preEnumRootsUs = 0;      // PreEnumRoots wall time: stop sweep, clear bitmaps, reclassify unswept.
    uint64_t markSetupUs = 0;         // EnumRoots wall time: handshake + EnumAndMarkRoots root discovery.
    uint64_t concurrentMarkUs = 0;    // Full ConcurrentMark phase wall time.
    uint64_t markPhaseTransitionUs = 0;
    uint64_t markPacingSetupUs = 0;
    uint64_t markPoolAcquireUs = 0;
    uint64_t markWorkerStartUs = 0;
    uint64_t markLoopUs = 0;
    uint64_t markQuiesceUs = 0;
    uint64_t markPoolReleaseUs = 0;
    uint64_t markSchedulerUs = 0;
    uint64_t markTerminationUs = 0;   // Full MarkTermination wall time, including STW acquire, IDLE transition and resume.
    uint64_t postMarkUs = 0;          // PostMarkTermination wall time: stats, threshold update and Sweep handoff.
    uint64_t postGcHookUs = 0;        // PostGarbageCollection hook wall time after UpdateGCStats completes.

    // ── Counting ──
    size_t rootsCount = 0;                // rootStaging_.size() after root enumeration.
    MSMarkTerminationReason markTerminationReason = MSMarkTerminationReason::NATURAL;
    uint64_t markMainWU = 0;
    uint64_t markMainActiveUs = 0;
    uint64_t markMainSlices = 0;
    uint64_t markHelperWU = 0;
    uint64_t markHelperActiveUs = 0;
    uint64_t markHelperSlices = 0;
    uint64_t markAssistWU = 0;
    uint64_t markAssistActiveUs = 0;
    uint64_t markAssistCalls = 0;
    uint64_t markAssistNoTaskCalls = 0;
    uint64_t markAssistAdmissionAccepted = 0;
    uint64_t markAssistAdmissionRejectedBacklog = 0;
    size_t markMaxHelpers = 0;
    size_t markInitialHelpers = 0;
    size_t markQuiesceResidualTasks = 0;

    // ── True live bytes (computed from markBitmaps after marking) ──
    size_t trueLiveBytes = 0;            // ComputeMarkedStats() result, used by UpdateGCStats and GC-end logs.

    // ── Floating-live threshold baseline ──
    size_t markStartInUseBytes = 0;
    size_t markEndInUseBytes = 0;
    size_t inUseBurstBytes = 0;
    size_t thresholdBaseBytes = 0;
    size_t outsideCentralSmallBytes = 0;
    size_t outsideLargeBytes = 0;
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

    // ── MarkNewObject: publish finalizer-registration/removal roots during Mark ──
    void MarkNewObject(BaseObject* obj) override;

    // ── Mutator assist interface (Step 2.2 §4) ──
    // Called from AllocBufferMS::Allocate() during concurrent marking.
    // Pays mutator assist debt in WorkUnits and returns paid WorkUnits.
    size_t MSMarkAssist(AllocBufferMS& allocBuffer, size_t debtWU);
    uint64_t GetAssistWorkPerByteScaled() const
    {
        return assistWorkPerByteScaled_.load(std::memory_order_acquire);
    }
    static constexpr uint64_t GetAssistRatioScale() { return kMSAssistRatioScale; }
    static MarkSweepCollector* GetActiveCollector()
    {
        return activeCollector_.load(std::memory_order_acquire);
    }
    // Persistent per-owner barrier producer API. Barrier publication is
    // deliberately excluded from cycleCompletedMarkWU_ pacing statistics.
    void AddManagedRefToBarrierBuffer(
        BaseObject* ref, MSBarrierMarkBuffer& buffer,
        uint32_t currentMarkEpoch, uintptr_t allocPlaneBase,
        uintptr_t markPlaneBase);
    void FlushBarrierMarkBuffer(MSBarrierMarkBuffer& buffer);
    void NotifyAllocBufferExit(AllocBufferMS& allocBuffer);
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
    void MergeRootsFromAllocBuffers(WorkStack& workStack) override;

    bool ShouldIgnoreRequest(GCRequest& request) override
    {
        return request.reason != GC_REASON_HEU && request.ShouldBeIgnored();
    }
    void UpdateGCStats() override;
private:
    // ── MS GC phases ──
    void DoGarbageCollection();
    void PreEnumRoots(bool worldStopped = false); // Step 2.1a: Stop sweep, clear bitmaps, reclassify
    void EnumRoots();            // Step 2.1b: Handshake + root enumeration
    void ConcurrentMark();      // Step 2.2: Concurrent marking (GC thread + assist)
    void MarkTermination(bool worldStopped = false); // Step 2.3: STW finalization of marking
    void PostMarkTermination(); // Step 2.4: Non-STW statistics, threshold update and Sweep handoff
    void RunOOMEmergencyCycle();

    // ── Step 2.1 internals ──
    void EnumAndMarkRoots();
    void MergeRootsFromAllocBuffers(std::vector<BaseObject*>& roots);
    bool DiscoverObject(BaseObject* obj, TypeInfo** markedTypeInfo = nullptr);
    enum class MSMarkRefSource : uint8_t {
        TRUSTED_MANAGED,
        TRUSTED_MAYBE_MANAGED,
        DEFENSIVE
    };
    template<MSMarkRefSource Source, typename MarkBatch,
             typename WorkCounter>
    void AddRefToMarkBatch(BaseObject* obj, MarkBatch& batch,
                           WorkCounter& work,
                           MSMarkPublicationContext& publication);
    template<bool Trusted, typename MarkBatch, typename WorkCounter>
    void AddResolvedRefToMarkBatchImpl(BaseObject* obj,
                                       const ResolvedMarkTarget& target,
                                       MarkBatch& batch, WorkCounter& work,
                                       MSMarkPublicationContext& publication);
    template<typename MarkBatch, typename WorkCounter>
    void AddMarkBitToBatch(uint32_t regionIdx, size_t wordIdx,
                           uint64_t bitMask, MarkBatch& batch,
                           WorkCounter& work,
                           MSMarkPublicationContext& publication);
    template<typename MarkBatch, typename WorkCounter>
    MSRegionInfo* FlushMarkWordEntryImpl(const MSMarkWordBatchEntry& entry,
                                         WorkCounter& work,
                                         const MarkBatch& batch);
    template<size_t Capacity>
    bool PublishPendingMarkRegions(
        MSPendingMarkRegionBatch<Capacity>& pending);
    void StageMarkPublication(MSRegionInfo* info, size_t wordIdx,
                              MSMarkPublicationContext& publication);
    void FlushMarkPublication(MSMarkPublicationContext& publication,
                              bool notifyReadyWorkers);
    template<typename MarkBatch, typename WorkCounter>
    void FlushMarkBatch(MarkBatch& batch, WorkCounter& work,
                        MSMarkPublicationContext& publication);
    void InitializeGCMarkBatch(MSMarkWordBatch& batch) const;
    void FlushAllBarrierMarkBuffersUnderSTW(
        CentralRegionPool& central, size_t& visitedBuffers,
        size_t& nonEmptyBuffers, size_t& bufferedEntries);
    void PrepareBarrierMarkBuffersForNewEpochUnderSTW(
        CentralRegionPool& central);
    void FoldGCMarkAssistTelemetry(AllocBufferMS& allocBuffer);
    void FoldActiveGCMarkAssistTelemetry();
    void PublishRootStaging();
    void PublishRootRange(const std::vector<BaseObject*>& roots,
                          size_t begin, size_t end);
    void RunSchedulerMarkLoop(uint64_t baseAssistRatio,
                              size_t maxAssistMultiplier,
                              MSMarkPressure pressure);
    void FinalizeConcurrentMarkAccounting();
    void RunSchedulerTask(MSTaskLease& lease, MSWorkUnitCounter& work);
    void RunRegionTask(MSTaskLease& lease, MSWorkUnitCounter& work);
    void RunObjectTask(MSTaskLease& lease, MSWorkUnitCounter& work);
    uint64_t DrainSchedulerUnderSTW();
    void VerifyMarkSchedulerQuiescence(const char* closureName);
    size_t GetCurrentMarkReadyShard() const;
    // Bitmap-only discovery. MSGC derives cycle object counts from a post-mark
    // bitmap walk instead of updating a hot global counter.
    // Diagnostic: after marking completes (in PostMarkTermination, before sweep
    // reclaims anything), walk all marked objects and report any ref edge that
    // points to an unmarked target. Such a target will be wrongly swept while
    // still reachable from a live object — the classic missed-mark UAF.
    // Gated by env cjMSVerifyMark; logs the first N violations only.
    void VerifyMarkCompleteness();

    // ── Step 2.2 internals ──
    void StartMarkWorkers(size_t maxHelpers, size_t initialHelpers);
    void RunMarkWorker(size_t workerId);
    size_t QuiesceMarkWorkers();

    // ── Step 2.3 internals ──
    struct MSTerminationDrainLaneStats {
        uint64_t workUnits = 0;
        uint64_t activeUs = 0;
        size_t completedTasks = 0;
        size_t waits = 0;
    };
    struct MSTerminationDrainStats {
        uint64_t workUnits = 0;
        uint64_t totalUs = 0;
        uint64_t setupUs = 0;
        uint64_t mainLaneUs = 0;
        uint64_t joinUs = 0;
        uint64_t startWorkWords = 0;
        size_t startTasks = 0;
        size_t helperCount = 0;
        bool parallel = false;
        MSTerminationDrainLaneStats main;
        MSTerminationDrainLaneStats helpers;
    };
    void RunTerminationDrainLaneUnderSTW(
        size_t participant, MSTerminationDrainLaneStats& stats);
    MSTerminationDrainStats DrainOrdinarySchedulerUnderSTW();
    void PrepareFinalizersForResurrection();
    void ClearWeakRefsAfterMark(size_t& cleared, size_t& preserved);

    // ── Step 2.4 internals ──
    void PrepareCycleRef();
    void PostResolveCycleTask();
    void CollectLargeGarbage();

    // ============================================================
    // Assist tuning parameters
    // ============================================================
    //
    // ── Root staging (populated and fully drained inside EnumRoots) ──
    std::vector<BaseObject*> rootStaging_;
    MSRootDedupTable rootDedupTable_;

    // Unified Region/Object mark scheduler. Ordinary small grey work is
    // represented by mark/scan planes plus Region tasks.
    MSMarkScheduler markScheduler_;
    size_t markReadyProcessorCount_ = 1;
    uint32_t markBatchEpoch_ = MSRegionInfo::NO_COLLECT_EPOCH;
    uintptr_t markBatchAllocPlaneBase_ = 0;
    uintptr_t markBatchMarkPlaneBase_ = 0;
    MSWorkUnitPacer markPacer_;
    std::atomic<uint64_t> cycleCompletedMarkWU_{0};

    // Scheduler progress epoch and helper control.
    WorkEpoch markWorkEpoch_;
    size_t markWorkerCount_ = 0;
    std::vector<uint64_t> markWorkerCompletedWU_;
    std::vector<uint64_t> markWorkerActiveUs_;
    std::vector<uint64_t> markWorkerSlices_;
    bool markPoolOwnedForCycle_ = false;
    MSMarkWorkerControl markWorkerControl_{&markWorkEpoch_};
    std::atomic<uint64_t> assistWorkPerByteScaled_{0};
    std::atomic<uint64_t> markAssistCompletedWU_{0};
    std::atomic<uint64_t> markAssistActiveUs_{0};
    std::atomic<uint64_t> markAssistCalls_{0};
    std::atomic<uint64_t> markAssistNoTaskCalls_{0};
    std::atomic<uint64_t> markAssistAdmissionAccepted_{0};
    std::atomic<uint64_t> markAssistAdmissionRejectedBacklog_{0};
    std::atomic<size_t> prevCycleMarkedObjects_{0};

    // ── Assist control ──
    static std::atomic<MarkSweepCollector*> activeCollector_;

    // (assist parameters are grouped above with other tuning constants)

    // ── DFX: Per-cycle stats ──
    MSGCCycleStats cycleStats_;
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
