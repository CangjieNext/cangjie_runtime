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
#include "Heap/Collector/TracingCollector.h"
#include "Heap/MCollector/MSAdaptiveHeapGrowthPolicy.h"
#include "Heap/MCollector/MSAssistPacing.h"
#include "Heap/MCollector/MSHeapThresholdPolicy.h"
#include "Heap/MCollector/MSMarkBatch.h"
#include "Heap/MCollector/MSRootSnapshot.h"
#include "Heap/MCollector/MSScavengeWatermarkPolicy.h"
#include "Heap/MCollector/MSMarkScheduler.h"
#include "Heap/MCollector/MSMarkWorkerControl.h"
#include "Heap/MCollector/MSWorkUnit.h"

namespace MapleRuntime {

class CentralRegionPool;
class AllocBufferMS;
class RegionSpaceMS;
struct MSBarrierMarkBuffer;
struct MSRegionInfo;
struct ResolvedMarkTarget;

// Per-GC-cycle accounting. Each main mark-cycle phase records its wall time;
// LogGCSummary emits the complete breakdown once at GC end.
struct MSGCCycleStats {
    uint64_t preEnumRootsUs = 0;
    uint64_t enumRootsUs = 0;
    uint64_t concurrentMarkUs = 0;
    uint64_t markTerminationStwUs = 0;
    uint64_t postMarkTerminationUs = 0;

    // ── True live bytes (computed from markBitmaps after marking) ──
    size_t trueLiveBytes = 0;            // Post-Mark bitmap statistics, used by UpdateGCStats and GC-end logs.

    // ── Floating-live threshold baseline ──
    size_t markStartInUseBytes = 0;
    // Snapshot immediately before PreEnum forces the previous Sweep epoch to
    // finish. It records the just-used Region footprint used to retain a
    // reusable cache after that Sweep makes dead Regions reclaimable.
    size_t preSweepInUseBytes = 0;

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
        return markAssistPacing_.GetPublishedRateScaled();
    }
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
    struct MSMarkWorkSnapshot {
        uint64_t mainWU = 0;
        uint64_t helperWU = 0;
        uint64_t assistWU = 0;

        uint64_t TotalWU() const
        {
            return SaturatingMSWUAdd(
                SaturatingMSWUAdd(mainWU, helperWU), assistWU);
        }
    };

    // Concurrent Mark has three independently executing producers. Keep the
    // WU accounting next to the phase owner so its one end-of-phase REPORT
    // line does not need task counters or a second scheduler traversal.
    struct MSConcurrentMarkWork {
        std::atomic<uint64_t> mainWU {0};
        std::atomic<uint64_t> helperWU {0};
        std::atomic<uint64_t> assistWU {0};

        void Reset()
        {
            mainWU.store(0, std::memory_order_relaxed);
            helperWU.store(0, std::memory_order_relaxed);
            assistWU.store(0, std::memory_order_relaxed);
        }

        void AccountMain(uint64_t completedWU)
        {
            mainWU.fetch_add(completedWU, std::memory_order_relaxed);
        }

        void AccountHelper(uint64_t completedWU)
        {
            helperWU.fetch_add(completedWU, std::memory_order_relaxed);
        }

        void AccountAssist(uint64_t completedWU)
        {
            assistWU.fetch_add(completedWU, std::memory_order_relaxed);
        }

        MSMarkWorkSnapshot Snapshot() const
        {
            return {mainWU.load(std::memory_order_acquire),
                    helperWU.load(std::memory_order_acquire),
                    assistWU.load(std::memory_order_acquire)};
        }
    };

    // The two STW closures reuse the same drain routine. This last-result
    // value is written and consumed only by the GC main thread while stopped.
    struct MSMarkDrainWork {
        uint64_t mainWU = 0;
        uint64_t helperWU = 0;
        size_t helperCount = 0;

        uint64_t TotalWU() const
        {
            return SaturatingMSWUAdd(mainWU, helperWU);
        }
    };

    struct MSHeapPolicyRuntimeState {
        MSAdaptiveHeapGrowthState adaptiveGrowth;
        uint64_t lastHeuGcStartUs = 0;
        // High is intentionally sticky across short GC-cycle fluctuations.
        // Low is recomputed directly from the current pre-sweep footprint.
        uint64_t lastScavengeHighDecayUs = 0;
    };

    struct MSHeapThresholdStateUpdate;
    struct MSScavengeWatermarkStateUpdate;

    MSHeapThresholdStateUpdate UpdateMSHeapThresholdState(
        RegionSpaceMS& space, const GCStats& gcStats, bool exceptionalBypass);
    MSScavengeWatermarkStateUpdate UpdateMSScavengeWatermarkState(
        RegionSpaceMS& space, const GCStats& gcStats,
        const MSHeapThresholdStateUpdate& heapThresholdState);

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
    void MergeRootsFromAllocBuffers(MSRootSnapshot& roots);
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
    MSMarkBitmapContext LoadCurrentMSMarkBitmapContext() const;
    void InitializeGCMarkBatch(MSMarkWordBatch& batch) const;
    void FlushAllBarrierMarkBuffersUnderSTW(CentralRegionPool& central);
    void FoldGCMarkAssistWork(AllocBufferMS& allocBuffer);
    void FoldActiveGCMarkAssistWork();
    void PublishRootStaging();
    void PublishRootRange(const std::vector<BaseObject*>& roots,
                          size_t begin, size_t end);
    void RunSchedulerMarkLoop(const MSMarkAssistPlan& assistPlan,
                              MSMarkPressure pressure);
    size_t AcquireMarkWorkerPool(bool& markPoolOwned);
    void UpdateMarkController(
        MSMarkControllerState& markController,
        const MSMarkAssistPlan& assistPlan, uint64_t outstanding,
        uint64_t outstandingWork,
        const MSMarkWorkerControl::Snapshot& control,
        uint64_t elapsedMarkUs);
    void FinishSchedulerMarkLoop(uint64_t mainCompletedWU,
                                 bool markPoolOwned);
    void FinalizeConcurrentMarkAccounting();
    void ResetCycleCompletedMarkWork();
    void AccountCycleCompletedMarkWork(uint64_t completedWU);
    uint64_t GetCycleCompletedMarkWork() const;
    void RunSchedulerTask(MSTaskLease& lease, MSWorkUnitCounter& work);
    void RunRegionTask(MSTaskLease& lease, MSWorkUnitCounter& work);
    void RunObjectTask(MSTaskLease& lease, MSWorkUnitCounter& work);
    void DrainSchedulerSeriallyUnderSTW();
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
    bool StartMarkWorkers(size_t maxHelpers);
    void RunMarkWorker(size_t workerId);
    void QuiesceMarkWorkers();

    // ── Step 2.3 internals ──
    void RunTerminationDrainLaneUnderSTW(
        size_t participant, uint64_t& completedWU);
    void DrainSchedulerWithHelpersUnderSTW();
    void PublishOrdinaryMarkTasksUnderSTW(CentralRegionPool& centralPool);
    void PublishFinalizerResurrectionRootsUnderSTW(MSWorkUnitCounter& resurrectionPublishWork);
    void PrepareFinalizersForResurrection();
    void ClearWeakRefsAfterMark();

    // ── Root staging (populated and fully drained inside EnumRoots) ──
    MSRootSnapshot rootSnapshot_;

    // Unified Region/Object mark scheduler. Ordinary small grey work is
    // represented by mark/scan planes plus Region tasks.
    MSMarkScheduler markScheduler_;
    size_t markReadyProcessorCount_ = 1;
    std::atomic<uint64_t> cycleCompletedMarkWU_{0};

    // ── Mark assist pacing and publication state ──
    // markAssistPacing_ owns the cross-cycle EWMA, next-cycle plan, pressure
    // gates, and all Mark assist rate/multiplier calculations. The collector
    // supplies measured work and normalized RuntimeParam values, then only
    // publishes the returned current-cycle rate to allocation buffers.
    //
    // AllocBufferMS reads the pacing-owned published rate with acquire semantics
    // while allocating and may call MSMarkAssist(). The ratio is reset to zero
    // before the phase ends, so mutators cannot assist outside concurrent mark.
    //
    // Assist work is first recorded in its owner AllocBufferMS; after assist
    // participants have quiesced, MSAssistPacing folds that work into the
    // cycle-wide WorkUnit total. activeCollector_ is the acquire/release
    // gateway that makes this collector reachable from the allocation path
    // only between PreEnumRoots setup and MarkTermination.
    MSMarkAssistPacing markAssistPacing_;
    static std::atomic<MarkSweepCollector*> activeCollector_;

    // Scheduler progress epoch and helper control.
    WorkEpoch markWorkEpoch_;
    std::unique_ptr<uint64_t[]> markWorkerCompletedWU_;
    size_t markWorkerCompletedCapacity_ = 0;
    size_t markWorkerCompletedCount_ = 0;
    bool markPoolOwnedForCycle_ = false;
    MSMarkWorkerControl markWorkerControl_{&markWorkEpoch_};
    MSConcurrentMarkWork concurrentMarkWork_;
    MSMarkDrainWork lastSTWDrainWork_;

    // EnumRoots is serial after the mutator handshake. These values describe
    // the single staging/publication boundary logged at phase end.
    size_t enumRootsRawCount_ = 0;
    size_t enumRootsUniqueCount_ = 0;
    size_t enumRootsScannedStackCount_ = 0;
    uint64_t enumRootsPublishedWU_ = 0;

    // ── Per-cycle accounting ──
    MSGCCycleStats cycleStats_;
    MSHeapPolicyRuntimeState heapPolicyState_;
    uint64_t currentGcStartUs_ = 0;

    void LogGCEntry(GCReason reason);
    void LogGCSummary(uint64_t gcTimeUs);
    void LogRegionSummary(const char* stage) const;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MARK_SWEEP_COLLECTOR_H
