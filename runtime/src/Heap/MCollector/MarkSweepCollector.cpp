// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MarkSweepCollector.h"
#include <cstdlib>
#include <cstring>
#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "CangjieRuntime.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSRegionManager.h"
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Heap/MBarrier/WriteBarrierBuffer.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Collector/ForwardDataManager.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"

#ifndef _WIN32
#include <unistd.h>  // usleep
#endif

namespace MapleRuntime {

namespace {
double ReadEnvPosDec(const char* name, double fallback)
{
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return fallback;
    }
    double value = CString::ParsePosDecFromEnv(env);
    return value <= 0 ? fallback : value;
}

size_t ReadEnvCount(const char* name, size_t fallback)
{
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return fallback;
    }
    size_t value = CString::ParsePosNumFromEnv(env);
    return value == 0 ? fallback : value;
}

double GetMsAsyncAllocDisableRatio()
{
    static const double value = ReadEnvPosDec("CJ_MS_ASYNC_ALLOC_DISABLE_RATIO", 2.0 / 3.0);
    return value;
}

size_t GetMsMinHeadroomRegions()
{
    static const size_t value = ReadEnvCount("CJ_MS_MIN_HEADROOM_REGIONS", 4);
    return value;
}

double GetMsMinHeadroomHeapRatio()
{
    static const double value = ReadEnvPosDec("CJ_MS_MIN_HEADROOM_HEAP_RATIO", 0.01);
    return value;
}

double GetMsThresholdHeapCapRatio()
{
    static const double value = ReadEnvPosDec("CJ_MS_THRESHOLD_HEAP_CAP_RATIO", 0.98);
    return value;
}

class WorkStackGreySink final : public MarkSweepCollector::GreyWorkSink {
public:
    explicit WorkStackGreySink(MarkSweepCollector::WorkStack& workStack) : workStack_(workStack) {}

    void PushGrey(BaseObject* obj) override
    {
        workStack_.push_back(obj);
    }

private:
    MarkSweepCollector::WorkStack& workStack_;
};
}

extern "C" void MSMarkBarrier_DumpAndResetCounters();

static bool IsHashMapTraceEnabled()
{
    static bool enabled = []() {
        const char* env = std::getenv("CJ_MS_HASHMAP_TRACE");
        return env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

static bool HashMapTracePermit()
{
    static size_t budget = []() -> size_t {
        const char* env = std::getenv("CJ_MS_HASHMAP_TRACE_LIMIT");
        if (env == nullptr || env[0] == '\0') {
            return 256;
        }
        long v = std::strtol(env, nullptr, 10);
        return v > 0 ? static_cast<size_t>(v) : 256;
    }();
    if (budget == 0) {
        return false;
    }
    --budget;
    return true;
}

static const char* GetDiagTypeName(const BaseObject* obj)
{
    if (obj == nullptr || obj->GetTypeInfo() == nullptr || obj->GetTypeInfo()->GetName() == nullptr) {
        return "<null>";
    }
    return obj->GetTypeInfo()->GetName();
}

static bool IsInterestingDiagType(const BaseObject* obj)
{
    const char* name = GetDiagTypeName(obj);
    return std::strstr(name, "HashMap") != nullptr ||
           std::strstr(name, "HashMapEntry") != nullptr ||
           std::strstr(name, "ArrayList") != nullptr ||
           std::strstr(name, "RawArray") != nullptr;
}

// Helper: GC tracing paths only treat main-heap addresses as managed.
static inline bool IsManagedAddress(MAddress addr)
{
    return Heap::IsHeapAddress(addr);
}

// Static member definition
std::atomic<MarkSweepCollector*> MarkSweepCollector::activeCollector_{nullptr};

// ============================================================================
// Lifecycle
// ============================================================================

void MarkSweepCollector::Init()
{
    LOG(RTLOG_REPORT, "[MS-GC] MarkSweepCollector::Init()");
    TracingCollector::Init();
    g_gcRequests[GC_REASON_HEU].SetMinInterval(CangjieRuntime::GetGCParam().gcInterval);
}

void MarkSweepCollector::Fini()
{
    LOG(RTLOG_REPORT, "[MS-GC] MarkSweepCollector::Fini()");
    TracingCollector::Fini();
}

void MarkSweepCollector::PreGarbageCollection(bool isConcurrent)
{
    GetGCStats().reason = gcReason;
    GetGCStats().async = !g_gcRequests[gcReason].IsSyncGC();
    GetGCStats().isConcurrentMark = isConcurrent;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    DumpBeforeGC();
#endif
    TRACE_COUNT("CJRT_pre_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

void MarkSweepCollector::PostGarbageCollection(uint64_t gcIndex)
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    TracingCollector::PostGarbageCollection(gcIndex);
    cycleStats_.postGcHookUs = TimeUtil::MicroSeconds() - t0;
}

// ============================================================================
// GC entry point
// ============================================================================

void MarkSweepCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    gcCycle_++;
    cycleStats_.Reset();
    SetGCReason(reason);

    GCStats& gcStats = GetGCStats();
    gcStats.reason = reason;
    gcStats.collectedBytes = 0;
    gcStats.gcStartTime = TimeUtil::NanoSeconds();

    // Block ①: GC Entry
    LogGCEntry(gcIndex, reason);

    // Block ④: Sweep summary from PREVIOUS cycle (output at start of new cycle)
    SnapshotPreviousCycleSweepStats();
    LogSweepSummary();

    PreGarbageCollection(/* isConcurrent */ true);
    DoGarbageCollection(gcIndex);
    UpdateGCStats();
    PostGarbageCollection(gcIndex);

    gcStats.gcEndTime = TimeUtil::NanoSeconds();
    uint64_t gcTimeNs = gcStats.gcEndTime - gcStats.gcStartTime;
    uint64_t gcTimeUs = gcTimeNs / NS_PER_US;

    // Block ⑤: Region statistics
    DumpRegionStatsMS();

    // Block ⑥: GC Summary
    LogGCSummary(gcTimeUs);

    prevCycleLargeFreedCount_ = cycleStats_.losFreedCount;
    prevCycleLargeFreedRegionCount_ = cycleStats_.losFreedRegionCount;
    prevCycleLargeFreedBytes_ = cycleStats_.losFreedBytes;

    g_gcCount++;
    g_gcTotalTimeUs += gcTimeUs;
    g_gcCollectedTotalBytes += gcStats.collectedBytes;
}

void MarkSweepCollector::DoGarbageCollection(uint64_t gcIndex)
{
    (void)gcIndex;

    PreEnumRoots();        // Step 2.1a
    EnumRoots();            // Step 2.1b
    ConcurrentMark();      // Step 2.2
    MarkTermination();     // Step 2.3
    PostMarkTermination(); // Step 2.4
    ConcurrentSweep();      // Step 4: concurrent sweep via gc-helper
}

// ============================================================================
// Step 2.1a: PreEnumRoots  (GC thread only, no STW)
//   Stop all sweeps, wait for in-flight bg/lazy sweep to quiesce, clear
//   markBitmaps, then stamp/reclassify any residual unswept regions as swept.
//   TL-region flush is deferred to MarkTermination's STW (FlushAll +
//   PrepareForSweep), because marking is address-based and does not
//   require regions to be in CentralRegionPool's lists.
// ============================================================================

void MarkSweepCollector::PreEnumRoots()
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- PreEnumRoots ---");

    // Set activeCollector_ for MSMarkAssist (Mutator assist during concurrent mark).
    activeCollector_.store(this, std::memory_order_release);

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    
    // Stop sweep → wait for in-flight sweep to quiesce → clear bitmaps
    // → reclassify residual unswept entries → re-enable.
    // Safe pre-STW: no SweepRegion running, mark-at-alloc inactive.
    if (centralPool != nullptr) {
        centralPool->StopAndWaitForAllSweep();
        centralPool->ClearAllMarkBitmaps();
        centralPool->ReclassifyUnsweptToSwept();
        centralPool->ReenableLazySweep();
    }
    prevCycleMarkedObjects_.store(markedObjectCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    // Reset marked object counter (GC-thread-only, no STW needed).
    markedObjectCount.store(0, std::memory_order_relaxed);

    cycleStats_.preEnumRootsUs = TimeUtil::MicroSeconds() - t0;
    VLOG(REPORT, "[MS-GC] PreEnumRoots: %sus",
         Pretty(cycleStats_.preEnumRootsUs).Str());
}

// ============================================================================
// Step 2.1b: EnumRoots  (handshake + root enumeration)
//   Phase B (handshake): TransitionToGCPhase(MS_MARK_SETUP) — each Mutator
//     scans its own stack roots at safepoint.
//   Phase C (GC thread): EnumAndMarkRoots — global roots + mark root set.
// ============================================================================

void MarkSweepCollector::EnumRoots()
{
    uint64_t phaseStart = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- EnumRoots ---");

    // Handshake: each Mutator reaches safepoint, scans its own stack roots.
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_MARK_SETUP, true);

    // GC thread enumerates global roots + marks root set.
    markWorkStack_.clear();
    {
        uint64_t t0 = TimeUtil::MicroSeconds();
        EnumAndMarkRoots(markWorkStack_.UnsafeRaw());
        cycleStats_.enumRootsUs = TimeUtil::MicroSeconds() - t0;
        cycleStats_.rootsCount = markWorkStack_.size();
    }

    cycleStats_.markSetupUs = TimeUtil::MicroSeconds() - phaseStart;
    VLOG(REPORT, "[MS-GC] EnumRoots: %sus, roots=%zu",
         Pretty(cycleStats_.markSetupUs).Str(), cycleStats_.rootsCount);
}

void MarkSweepCollector::ClearAllMarkBitmaps()
{
    // Mark bitmaps are now cleared in MarkSetup via CentralRegionPool::VisitAllRegions.
    // This function is a no-op stub for interface compatibility.
}

void MarkSweepCollector::EnsureSweepDone()
{
    // Sweep completion is now ensured in MarkSetup via StopAndWaitForAllSweep().
    // This function is a no-op stub for interface compatibility.
}

// ============================================================================
// MarkObject override: write to MSRegionInfo::markBitmap for MS regions,
// return already-marked/external status for non-heap addresses.
// ============================================================================
bool MarkSweepCollector::MarkObject(BaseObject* obj) const
{
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);

    if (MSRegionInfoArray::IsHeapAddress(addr)) {
        return MSRegionInfoArray::MarkObjectInHeap(addr);
    }

    return true;
}

bool MarkSweepCollector::DiscoverObject(BaseObject* obj)
{
    // Guard against stale pointers to reclaimed/zeroed slots.
    // For such slots stateWord->TypeInfo is null, and they must not be enqueued
    // into mark work queues in concurrent mark.
    if (UNLIKELY(obj == nullptr || !obj->IsValidObject())) {
        return false;
    }

    if (MarkObject(obj)) {
        return false;
    }
    (void)markedObjectCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool MarkSweepCollector::TryEnqueueGreyObject(BaseObject* obj, GreyWorkSink& sink)
{
    if (!DiscoverObject(obj)) {
        return false;
    }
    sink.PushGrey(obj);
    return true;
}

bool MarkSweepCollector::TryEnqueueGreyObject(BaseObject* obj, GreyWorkQueue& queue)
{
    if (!DiscoverObject(obj)) {
        return false;
    }
    queue.PushNewlyGrey(obj);
    return true;
}

void MarkSweepCollector::DiscoverAndPushObject(BaseObject* obj, WorkStack& workStack)
{
    WorkStackGreySink sink(workStack);
    (void)TryEnqueueGreyObject(obj, sink);
}

void MarkSweepCollector::DebugVerifyQueuedGreyObject(const BaseObject* obj, const char* queueName) const
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    CHECK_DETAIL(IsMarkedObject(obj),
                 "[MS-GC] protocol violation: queue %s contains unmarked obj=%p",
                 queueName, static_cast<const void*>(obj));
#else
    (void)obj;
    (void)queueName;
#endif
}


void MarkSweepCollector::EnumAndMarkRoots(WorkStack& workStack)
{
    // MSGC does not need the parallel root-enumeration thread pool used by
    // CopyGC. Enumerate all roots serially into a single work stack.
    EnumAllCommonRootsSerial(workStack);
    EnumAllExportRoots(workStack);
}

void MarkSweepCollector::MergeRootsFromAllocBuffers(WorkStack& workStack)
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        WorkStack mutatorRoots = NewWorkStack();
        centralPool->VisitAllocBufferMS([&mutatorRoots](AllocBufferMS& buffer) { buffer.MergeRoots(mutatorRoots); });
        while (!mutatorRoots.empty()) {
            BaseObject* obj = mutatorRoots.back();
            mutatorRoots.pop_back();
            DiscoverAndPushObject(obj, workStack);
        }
    }
}

// ============================================================================
// Step 2.2: ConcurrentMark
// ============================================================================

// Forward declaration (defined later, used in ConcurrentMark and MSMarkAssist)
static bool IsAssistDisabledByEnv();

static inline void ConsumeAssistRemaining(std::atomic<int64_t>& remaining, size_t done)
{
    if (done == 0) {
        return;
    }
    int64_t cur = remaining.load(std::memory_order_relaxed);
    while (cur > 0) {
        int64_t dec = std::min(cur, static_cast<int64_t>(done));
        if (remaining.compare_exchange_weak(cur, cur - dec,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
            break;
        }
    }
}

void MarkSweepCollector::ConcurrentMark()
{
    // activeCollector_ was already set in MarkSetup (before STW).

    // 8. Transition to concurrent marking phase
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_CONCURRENT_MARK, true);

    // 9. Reset assist stats and bgScanCredit
    assistMarkedCount_.store(0, std::memory_order_relaxed);
    assistCallTotal_.store(0, std::memory_order_relaxed);
    assistProcessedCount_.store(0, std::memory_order_relaxed);
    assistTracedCount_.store(0, std::memory_order_relaxed);
    assistFastSkipCount_.store(0, std::memory_order_relaxed);
    globalShareCount_.store(0, std::memory_order_relaxed);
    globalSharedBufCount_.store(0, std::memory_order_relaxed);
    globalPopHitCount_.store(0, std::memory_order_relaxed);
    globalPopMissCount_.store(0, std::memory_order_relaxed);
    bgScanCredit_.store(0, std::memory_order_relaxed);
    bgScanCreditConsumed_.store(0, std::memory_order_relaxed);

    // Go-style gcAssistRatio: estimate scan work per allocated byte.
    // ratioScaled = ceil(prevCycleMarkedObjects * scale / allocBudgetBytes)
    GCStats& gcStats = GetGCStats();
    size_t threshold = gcStats.GetThreshold();
    size_t liveBaseline = gcStats.lastTrueLiveBytes;
    size_t allocBudgetBytes = (threshold > liveBaseline) ? (threshold - liveBaseline) : 0;
    size_t scanEstimateObjects = prevCycleMarkedObjects_.load(std::memory_order_relaxed);
    uint64_t ratioScaled = 0;
    if (allocBudgetBytes > 0 && scanEstimateObjects > 0) {
        ratioScaled = (static_cast<uint64_t>(scanEstimateObjects) * kAssistRatioScale +
                       static_cast<uint64_t>(allocBudgetBytes) - 1) /
                      static_cast<uint64_t>(allocBudgetBytes);
    }
    assistWorkPerByteScaled_.store(ratioScaled, std::memory_order_release);
    scanWorkRemaining_.store(static_cast<int64_t>(scanEstimateObjects), std::memory_order_relaxed);
    assistThresholdBytes_ = threshold;
    assistLiveBaselineBytes_ = liveBaseline;
    assistAllocBaselineBytes_ = gcStats.baselineAllocatedBytes;
    VLOG(REPORT,
         "[MS-GC] assist ratio init: prevMarked=%zu allocBudget=%zuB ratio=%zu/%zu obj-per-byte",
         scanEstimateObjects,
         allocBudgetBytes,
         static_cast<size_t>(ratioScaled),
         static_cast<size_t>(kAssistRatioScale));

    // 10. Init Per-P GCWork (Step5): initialize gcThreadWork_ and all per-P gcWork
    gcThreadWork_.Init(&globalFull_, &workBufPool_);
    InitAllPerPGCWork();

    // 11. Enable assist (skip if disabled by env CJ_MS_ASSIST_DISABLE=1)
    if (!IsAssistDisabledByEnv()) {
        assistEnabled_.store(true, std::memory_order_release);
    }

    // 12. Transfer root objects from markWorkStack_ to gcThreadWork_
    TransferRootsToGCWork();

    // 13. Run concurrent marking loop (GC thread + Mutator assist)
    RunConcurrentMarkLoop();

    // 14. Disable Mutator assist and clear activeCollector_.
    assistEnabled_.store(false, std::memory_order_release);
    assistWorkPerByteScaled_.store(0, std::memory_order_release);
    scanWorkRemaining_.store(0, std::memory_order_release);
}

void MarkSweepCollector::RunConcurrentMarkLoop()
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- ConcurrentMark (Step5: Per-P gcWork + LFStack) ---");

    convergenceStableRounds_ = 0;
    size_t localCreditAccum = 0;       // Opt2: batch bgScanCredit accumulation

    while (true) {
        bool assistActive = assistEnabled_.load(std::memory_order_acquire);

        // 1. Drain a slice from gcThreadWork_
        size_t scannedCount = 0;
        DrainGCWork(gcThreadWork_, kSliceMaxObjects, scannedCount);
        ConsumeAssistRemaining(scanWorkRemaining_, scannedCount);

        // Opt2: Always accumulate bgScanCredit from GC thread's work.
        // When GC thread is scanning fast enough, bgScanCredit > 0 lets
        // Mutator assist calls skip work via TryStealBgCredit() — this is
        // the key pacing mechanism preventing assist from over-throttling
        // mutator throughput.
        localCreditAccum += scannedCount;
        if (localCreditAccum >= kCreditFlushThreshold) {
            bgScanCredit_.fetch_add(static_cast<int64_t>(localCreditAccum), std::memory_order_release);

            size_t allocatedBytes = Heap::GetHeap().GetAllocator().AllocatedBytes();
            size_t newAlloc = (allocatedBytes > assistAllocBaselineBytes_) ?
                              (allocatedBytes - assistAllocBaselineBytes_) : 0;
            size_t effectiveAlloc = assistLiveBaselineBytes_ + newAlloc;
            size_t remainingAllocBudget = (assistThresholdBytes_ > effectiveAlloc) ?
                                          (assistThresholdBytes_ - effectiveAlloc) : 0;
            int64_t remainingScanSigned = scanWorkRemaining_.load(std::memory_order_acquire);
            size_t remainingScan = (remainingScanSigned > 0) ? static_cast<size_t>(remainingScanSigned) : 0;
            uint64_t dynamicRatioScaled = 0;
            if (remainingAllocBudget > 0 && remainingScan > 0) {
                dynamicRatioScaled =
                    (static_cast<uint64_t>(remainingScan) * kAssistRatioScale +
                     static_cast<uint64_t>(remainingAllocBudget) - 1) /
                    static_cast<uint64_t>(remainingAllocBudget);
            }
            assistWorkPerByteScaled_.store(dynamicRatioScaled, std::memory_order_release);

            localCreditAccum = 0;
        }

        // 2. No explicit work sharing needed — assist threads naturally
        //    pick up objects from globalFull_, which receives work from
        //    WriteBarrierBuffer overflow (FlushWriteBarrierOverflow).
        //    DFS frontier in gcThreadWork_ stays narrow (4-8 objects with
        //    avg 1.0 children), so splitting has negligible benefit.

        if (gcThreadWork_.Empty() && globalFull_.Empty()) {
            convergenceStableRounds_++;
            if (convergenceStableRounds_ >= kConvergenceRequiredRounds) {
                // ── Pre-convergence per-P flush ──
                // Disable assist to prevent races on per-P buffers, then
                // flush all per-P gcWork to global.  If residual work is
                // found, drain it here (concurrent) instead of deferring
                // to MarkTermination STW.
                assistEnabled_.store(false, std::memory_order_release);
                usleep(200);  // let in-flight assist calls complete

                DisposeAllPerPGCWork();

                if (!globalFull_.Empty()) {
                    // Per-P flush produced residual work — continue draining.
                    // Assist stays disabled to avoid re-accumulation.
                    VLOG(REPORT, "[MS-GC] Pre-convergence flush: residual per-P work found, re-draining");
                    convergenceStableRounds_ = 0;
                    continue;
                }
                // No residual work → true convergence (assist already disabled)
                // Opt2: flush remaining local credit before exiting
                if (localCreditAccum > 0) {
                    bgScanCredit_.fetch_add(static_cast<int64_t>(localCreditAccum), std::memory_order_release);
                    localCreditAccum = 0;
                }
                break;
            }
            // Short yield — let in-flight assist finish pushing results
            usleep(50);
        } else {
            convergenceStableRounds_ = 0;
        }
    }

    // Snapshot assist stats
    cycleStats_.assistCallCount = assistCallTotal_.load(std::memory_order_relaxed);
    cycleStats_.assistMarkedObjects = assistMarkedCount_.load(std::memory_order_relaxed);
    cycleStats_.assistProcessedObjects = assistProcessedCount_.load(std::memory_order_relaxed);
    cycleStats_.assistTracedObjects = assistTracedCount_.load(std::memory_order_relaxed);
    cycleStats_.assistFastSkipCount = assistFastSkipCount_.load(std::memory_order_relaxed);
    cycleStats_.globalShareCount = globalShareCount_.load(std::memory_order_relaxed);
    cycleStats_.globalSharedBufs = globalSharedBufCount_.load(std::memory_order_relaxed);
    cycleStats_.globalPopHitCount = globalPopHitCount_.load(std::memory_order_relaxed);
    cycleStats_.globalPopMissCount = globalPopMissCount_.load(std::memory_order_relaxed);
    cycleStats_.bgScanCreditConsumed = bgScanCreditConsumed_.load(std::memory_order_relaxed);

    cycleStats_.concurrentMarkUs = TimeUtil::MicroSeconds() - t0;
    VLOG(REPORT,
         "[MS-GC] ConcurrentMark time: %sus, marked %zu objects "
         "(assist: %zu calls, %zu marked, %zu processed, %zu traced, %zu bgCredit-skip; "
         "LFStack: push %zu, pop hit %zu miss %zu, bgCredit consumed %zu)",
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         markedObjectCount.load(std::memory_order_relaxed),
         cycleStats_.assistCallCount,
         cycleStats_.assistMarkedObjects,
         cycleStats_.assistProcessedObjects,
         cycleStats_.assistTracedObjects,
         cycleStats_.assistFastSkipCount,
         cycleStats_.globalShareCount,
         cycleStats_.globalPopHitCount,
         cycleStats_.globalPopMissCount,
         cycleStats_.bgScanCreditConsumed);
}

// ============================================================================
// DrainGCWork: drain objects from PerPGCWork, trace children back into it.
// This is the Step5 replacement for RunCollectorDrainSlice.
// ============================================================================
void MarkSweepCollector::DrainGCWork(PerPGCWork& gcw, size_t maxObjects,
                                     size_t& scannedCount)
{
    scannedCount = 0;
    for (size_t i = 0; i < maxObjects; i++) {
        BaseObject* obj = gcw.TryGetObj();
        if (obj == nullptr) {
            break;
        }

        if (UNLIKELY(!obj->IsValidObject())) {
            LOG(RTLOG_FATAL, "[MS-GC] DrainGCWork: FATAL invalid stateWord obj=%p, "
                "stateWord=0x%zx, markBit=%d",
                (void*)obj,
                *reinterpret_cast<uintptr_t*>(obj),
                (int)IsMarkedObject(obj));
        }

        scannedCount++;

        if (!obj->HasRefField()) {
            continue;
        }

        if (UNLIKELY(obj->IsWeakRef())) {
            RefField<>* referentField =
                reinterpret_cast<RefField<>*>(reinterpret_cast<uintptr_t>(obj) + sizeof(uintptr_t));
            BaseObject* referent = GetAndTryTagObj(obj, *referentField);
            if (referent != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(referent))) {
                WeakRefBuffer::Instance().Insert(obj);
            }
            continue;
        }

        TraceObjectRefFieldsToGCWork(obj, gcw);
    }
}

// ============================================================================
// TraceObjectRefFieldsToGCWork: trace all ref fields of obj, push grey children
// into PerPGCWork (Step5 replacement for TraceObjectRefFields on WorkStack).
// ============================================================================
void MarkSweepCollector::TraceObjectRefFieldsToGCWork(BaseObject* obj, PerPGCWork& gcw)
{
    auto refFunc = [this, &gcw](RefField<>& field) {
        BaseObject* target = field.GetTargetObject();
        if (target != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(target))) {
            if (DiscoverObject(target)) {
                gcw.PutObj(target);
            }
        }
    };
    obj->ForEachRefField(refFunc);
}

// ============================================================================
// TransferRootsToGCWork: move root objects from markWorkStack_ into gcThreadWork_.
// Called once at start of ConcurrentMark after root enumeration.
// ============================================================================
void MarkSweepCollector::TransferRootsToGCWork()
{
    size_t transferred = 0;
    while (!markWorkStack_.empty()) {
        BaseObject* obj = markWorkStack_.back();
        markWorkStack_.pop_back();
        gcThreadWork_.PutObj(obj);
        transferred++;
    }
    VLOG(REPORT, "[MS-GC] TransferRootsToGCWork: %zu root objects → gcThreadWork_", transferred);
}

void MarkSweepCollector::TraceRefField(
    BaseObject* obj, RefField<>& field, WorkStack& workStack)
{
    BaseObject* target = field.GetTargetObject();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    bool targetMarked = IsMarkedObject(target);
    if (!targetMarked) {
        DiscoverAndPushObject(target, workStack);
    }
#else
    DiscoverAndPushObject(target, workStack);
#endif
}

// ============================================================================
// Global work queue helpers (Step5: LFStack-based, lock-free)
// ============================================================================

void MarkSweepCollector::FlushWriteBarrierOverflow(WriteBarrierBuffer& wbBuf)
{
    // Flush WB buffer objects into a WorkBuf and push to global LFStack.
    WorkBuf* buf = workBufPool_.Get();
    size_t n = wbBuf.Count();
    BaseObject** data = wbBuf.Data();
    size_t pushed = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] != nullptr) {
            if (buf->Full()) {
                globalFull_.Push(buf);
                buf = workBufPool_.Get();
            }
            buf->Push(data[i]);
            pushed++;
        }
    }
    wbBuf.Clear();
    if (!buf->Empty()) {
        globalFull_.Push(buf);
    } else {
        workBufPool_.Put(buf);
    }
    // Fix 3: WriteBarrier-produced grey objects represent new scan work that was
    // not accounted for in scanWorkRemaining_ (initialized from prevCycleMarkedObjects_).
    // Without this update, scanWorkRemaining_ reaches zero prematurely and the dynamic
    // ratio drops to zero, causing assist to stop accumulating debt even though there
    // are still grey WB objects in globalFull_ waiting to be scanned.
    if (pushed > 0 && assistWorkPerByteScaled_.load(std::memory_order_relaxed) > 0) {
        scanWorkRemaining_.fetch_add(static_cast<int64_t>(pushed), std::memory_order_relaxed);
    }
}

void MarkSweepCollector::PushSingleToDeferredMainThread(BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(deferredMainThreadMutex_);
    deferredMainThreadQueue_.PushMovedGrey(obj);
}

// ============================================================================
// Step5: LFStack helpers
// ============================================================================

void MarkSweepCollector::DrainGlobalLFStackToMainQueue(GreyWorkQueue& mainQueue)
{
    while (true) {
        WorkBuf* buf = globalFull_.Pop();
        if (buf == nullptr) {
            break;
        }
        for (size_t i = 0; i < buf->len; i++) {
            mainQueue.PushMovedGrey(buf->objs[i]);
        }
        workBufPool_.Put(buf);
    }
}

size_t MarkSweepCollector::TryStealBgCreditPartial(size_t debtObjects)
{
    if (debtObjects == 0) {
        return 0;
    }

    int64_t credit = bgScanCredit_.load(std::memory_order_acquire);
    if (credit <= 0) {
        return 0;
    }
    int64_t stolen = std::min(static_cast<int64_t>(debtObjects), credit);
    while (!bgScanCredit_.compare_exchange_weak(
               credit, credit - stolen,
               std::memory_order_release, std::memory_order_relaxed)) {
        if (credit <= 0) {
            return 0;
        }
        stolen = std::min(static_cast<int64_t>(debtObjects), credit);
    }
    bgScanCreditConsumed_.fetch_add(static_cast<size_t>(stolen), std::memory_order_relaxed);
    return static_cast<size_t>(stolen);
}

void MarkSweepCollector::InitAllPerPGCWork()
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->VisitAllocBufferMS([this](AllocBufferMS& buf) {
            buf.GetGCWork().Init(&globalFull_, &workBufPool_);
        });
    }
}

void MarkSweepCollector::DisposeAllPerPGCWork()
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->VisitAllocBufferMS([](AllocBufferMS& buf) {
            if (buf.GetGCWork().IsInitialized()) {
                buf.GetGCWork().Dispose();
            }
        });
    }
    // Also dispose the GC thread's own work
    gcThreadWork_.Dispose();
}

void MarkSweepCollector::ResetAllPerPGCWork()
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->VisitAllocBufferMS([](AllocBufferMS& buf) {
            if (buf.GetGCWork().IsInitialized()) {
                buf.GetGCWork().Reset();
            }
        });
    }
    gcThreadWork_.Reset();
}

// ============================================================================
// MSMarkAssist (Step5): uses per-P gcWork from AllocBufferMS + bgScanCredit.
// Budget-limited: processes at most kAssistMaxObjectsPerCall grey objects.
// ============================================================================

static bool IsAssistDisabledByEnv()
{
    static bool disabled = []() {
        const char* env = std::getenv("CJ_MS_ASSIST_DISABLE");
        return env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
    }();
    return disabled;
}

size_t MarkSweepCollector::MSMarkAssist(size_t debtObjects)
{
    if (debtObjects == 0) {
        return 0;
    }

    if (!assistEnabled_.load(std::memory_order_acquire)) {
        return 0;
    }

    // env var kill-switch: CJ_MS_ASSIST_DISABLE=1 disables all assist work
    if (IsAssistDisabledByEnv()) {
        return 0;
    }

    assistCallTotal_.fetch_add(1, std::memory_order_relaxed);

    // Step5: Try to steal bgScanCredit first — if GC thread is ahead, skip/partially skip work.
    size_t paidByCredit = TryStealBgCreditPartial(debtObjects);
    if (paidByCredit >= debtObjects) {
        assistFastSkipCount_.fetch_add(1, std::memory_order_relaxed);
        return debtObjects;
    }
    size_t assistBudget = std::min(kAssistMaxObjectsPerCall, debtObjects - paidByCredit);
    if (assistBudget == 0) {
        return paidByCredit;
    }

    // Get per-P gcWork from this thread's AllocBufferMS.
    AllocBufferMS* allocBuf = ThreadLocal::GetAllocBufferMS();
    if (allocBuf == nullptr) {
        return paidByCredit;
    }
    PerPGCWork& gcw = allocBuf->GetGCWork();

    // Lazy init: if AllocBufferMS was created after ConcurrentMark started
    if (!gcw.IsInitialized()) {
        gcw.Init(&globalFull_, &workBufPool_);
    }

    // If per-P gcWork is empty and global is also empty, nothing to do.
    if (gcw.Empty() && globalFull_.Empty()) {
        return paidByCredit;
    }

    size_t nProcessed = 0;
    size_t nTraced = 0;
    for (size_t i = 0; i < assistBudget; i++) {
        // Opt1: Check assistEnabled_ every 128 iterations so this mutator
        // can reach a safepoint promptly when GC needs to start a new cycle.
        if ((i & 127) == 0 && i > 0) {
            if (!assistEnabled_.load(std::memory_order_acquire)) {
                break;
            }
        }

        BaseObject* obj = gcw.TryGetObj();
        if (obj == nullptr) {
            break;
        }

        if (UNLIKELY(!obj->IsValidObject())) {
            LOG(RTLOG_FATAL, "[MS-GC] MSMarkAssist: FATAL invalid stateWord obj=%p, "
                "stateWord=0x%zx, markBit=%d",
                (void*)obj,
                *reinterpret_cast<uintptr_t*>(obj),
                (int)IsMarkedObject(obj));
        }

        nProcessed++;

        if (!obj->HasRefField()) {
            continue;
        }

        // WeakRef: defer to GC main thread
        if (UNLIKELY(obj->IsWeakRef())) {
            PushSingleToDeferredMainThread(obj);
            continue;
        }

        // Trace children → push back into same per-P gcWork
        TraceObjectRefFieldsToGCWork(obj, gcw);
        nTraced++;
    }

    assistProcessedCount_.fetch_add(nProcessed, std::memory_order_relaxed);
    assistTracedCount_.fetch_add(nTraced, std::memory_order_relaxed);
    ConsumeAssistRemaining(scanWorkRemaining_, nProcessed);
    return paidByCredit + nProcessed;
}

// ============================================================================
// Step 2.3: MarkTermination (STW)
// ============================================================================

void MarkSweepCollector::MarkTermination()
{
    uint64_t phaseStart = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- MarkTermination (STW) ---");

    // STW: freeze all mutators
    ScopedStopTheWorld stw("MS MarkTermination", true, GCPhase::GC_PHASE_MS_MARK_TERMINATION);

    // Now that all mutators are frozen, clear activeCollector_.
    activeCollector_.store(nullptr, std::memory_order_release);

    GreyWorkQueue mainQueue;
    size_t directSourceEntries = 0;
    size_t pendingWorkEntries = 0;
    size_t resurrectionEntries = 0;

    // 1. Collect direct mutator/barrier sources:
    //    WriteBarrierBuffer only.
    {
        size_t before = mainQueue.size();
        FlushAllWriteBarrierBuffers(mainQueue);
        (void)before;

        directSourceEntries = mainQueue.size();
    }

    // 2. Collect pending concurrent-mark work:
    //    Step5: Dispose all per-P gcWork + global LFStack +
    //    deferred main-thread-only special work.
    {
        DisposeAllPerPGCWork();
        DrainGlobalLFStackToMainQueue(mainQueue);
        DrainDeferredMainThreadQueueToMainQueue(mainQueue);
        pendingWorkEntries = mainQueue.size() - directSourceEntries;
    }

    // 3. Collect resurrection/finalizer-related candidates.
    {
        CollectResurrectionCandidatesToMain(mainQueue);
        resurrectionEntries = mainQueue.size() - (directSourceEntries + pendingWorkEntries);
    }

    // 4. Drain main queue with Execute-like loop (same as concurrent mark logic)
    {
        size_t beforeMark = markedObjectCount.load(std::memory_order_relaxed);
        ExecuteLikeDrainMainQueue(mainQueue);
        cycleStats_.markTermNewlyMarkedObjects =
            markedObjectCount.load(std::memory_order_relaxed) - beforeMark;
    }

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        // Experimental policy: keep thread-local regions out of the current
        // sweep generation; they will be returned later and stamped into the
        // new generation when flowing back to the central pool.
        centralPool->PrepareForSweep();
    }

    // Step5: Reset all per-P gcWork and return WorkBufs to pool.
    ResetAllPerPGCWork();

    cycleStats_.wbBufEntriesTotal = directSourceEntries + pendingWorkEntries;
    cycleStats_.wbBufFlushUs = 0;
    cycleStats_.drainMarkQueueUs = 0;
    cycleStats_.markTerminationUs = TimeUtil::MicroSeconds() - phaseStart;
    MSMarkBarrier_DumpAndResetCounters();
    VLOG(REPORT,
         "[MS-GC] MarkTermination total: %sus, flushed=%zu entries, resurrection=%zu entries, "
         "processed=%zu objects, newlyMarked=%zu",
         Pretty(cycleStats_.markTerminationUs).Str(),
         directSourceEntries + pendingWorkEntries,
         resurrectionEntries,
         cycleStats_.markTermProcessedObjects,
         cycleStats_.markTermNewlyMarkedObjects);

    // Transition to MS_SWEEP INSIDE STW so that when mutators resume,
    // the phase is MS_SWEEP (idle barrier). This eliminates a gap where
    // mutators could still buffer write-barrier entries after the last drain
    // but before PostMarkTermination switches the phase.
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_SWEEP, false);
}

void MarkSweepCollector::CollectUnmarkedToMainQueue(GreyWorkQueue& mainQueue)
{
    // Collect extern (foreign) object candidates
    CollectExternCandidatesToMain(mainQueue);

    // Collect finalizer resurrection candidates
    CollectResurrectionCandidatesToMain(mainQueue);
}

void MarkSweepCollector::FlushAllWriteBarrierBuffers(GreyWorkQueue& mainQueue)
{
    auto func = [&mainQueue](Mutator& mutator) {
        mutator.GetWriteBarrierBuffer().Flush(mainQueue.UnsafeRaw());
    };
    MutatorManager::Instance().VisitAllMutators(func);
}

void MarkSweepCollector::DrainDeferredMainThreadQueueToMainQueue(GreyWorkQueue& mainQueue)
{
    std::lock_guard<std::mutex> lock(deferredMainThreadMutex_);
    mainQueue.insert(deferredMainThreadQueue_);
}

void MarkSweepCollector::CollectExternCandidatesToMain(GreyWorkQueue& mainQueue)
{
    for (auto& kv : discoveredExternObjects) {
        for (BaseObject* obj : kv.second) {
            if (obj != nullptr && obj->IsValidObject() &&
                IsManagedAddress(reinterpret_cast<MAddress>(obj))) {
                (void)TryEnqueueGreyObject(obj, mainQueue);
            }
        }
    }
}

void MarkSweepCollector::CollectResurrectionCandidatesToMain(GreyWorkQueue& mainQueue)
{
    VisitFinalizerRoots([&mainQueue, this](ObjectRef& ref) {
        BaseObject* finalizerObj = ref.object;
        if (finalizerObj != nullptr && finalizerObj->IsValidObject() &&
            IsManagedAddress(reinterpret_cast<MAddress>(finalizerObj))) {
            (void)TryEnqueueGreyObject(finalizerObj, mainQueue);
        }
    });
}

void MarkSweepCollector::ExecuteLikeDrainMainQueue(GreyWorkQueue& mainQueue)
{
    size_t processed = 0;

    while (!mainQueue.empty()) {
        BaseObject* obj = mainQueue.back();
        mainQueue.pop_back();
        processed++;
        // Guard: objects in released/reclaimed regions must not appear in
        // the mark queue. A zeroed stateWord is a fatal GC invariant violation.
        if (!obj->IsValidObject()) {
            LOG(RTLOG_FATAL, "[MS-GC] ExecuteLikeDrainMainQueue: FATAL invalid stateWord obj=%p, "
                "stateWord=0x%zx",
                (void*)obj,
                *reinterpret_cast<uintptr_t*>(obj));
        }

        if (!obj->HasRefField()) {
            continue;
        }

        if (UNLIKELY(obj->IsWeakRef())) {
            RefField<>* referentField =
                reinterpret_cast<RefField<>*>(reinterpret_cast<uintptr_t>(obj) + sizeof(uintptr_t));
            BaseObject* referent = GetAndTryTagObj(obj, *referentField);
            if (referent != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(referent))) {
                TraceObjectRefFields(referent, mainQueue.UnsafeRaw());
                WeakRefBuffer::Instance().Insert(obj);
            }
            continue;
        }

        TraceObjectRefFields(obj, mainQueue.UnsafeRaw());
    }

    cycleStats_.markTermProcessedObjects = processed;
}

// ============================================================================
// Step 2.4: PostMarkTermination (non-STW, concurrent-safe)
// ============================================================================

void MarkSweepCollector::PostMarkTermination()
{
    uint64_t phaseStart = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- PostMark ---");

    // Phase transition and WB clear already done in MarkTermination (inside STW).
    // No need to transition again or clear WB buffers here.

    // 1. Weak reference cleanup: referent not marked → set to null
    {
        uint64_t t0 = TimeUtil::MicroSeconds();
        WeakRefBuffer::Instance().ClearWeakRefBuffer();
        uint64_t weakRefUs = TimeUtil::MicroSeconds() - t0;
        VLOG(REPORT, "[MS-GC] ProcessWeakRefs: %sus", Pretty(weakRefUs).Str());
    }

    // 2. Finalizer processing: enqueue unreachable finalizer objects
    {
        uint64_t t0 = TimeUtil::MicroSeconds();
        ProcessFinalizers();
        uint64_t finalizerUs = TimeUtil::MicroSeconds() - t0;
        VLOG(REPORT, "[MS-GC] EnqueueFinalizers: enqueued %u objects, %sus",
             snapshotFinalizerNum, Pretty(finalizerUs).Str());
        cycleStats_.finalizerEnqueued = snapshotFinalizerNum;
    }

    // 6. Large object garbage collection (eager sweep for in-heap large regions) — with stats
    {
        uint64_t t0 = TimeUtil::MicroSeconds();
        CollectLargeGarbage();
        cycleStats_.losCollectUs = TimeUtil::MicroSeconds() - t0;
        VLOG(REPORT, "[MS-GC] CollectLargeGarbage: freed %zu large objects / %zu regions (%s B), %sus",
             cycleStats_.losFreedCount, cycleStats_.losFreedRegionCount,
             Pretty(cycleStats_.losFreedBytes).Str(),
             Pretty(cycleStats_.losCollectUs).Str());
    }

    cycleStats_.postMarkUs = TimeUtil::MicroSeconds() - phaseStart;
    VLOG(REPORT, "[MS-GC] PostMark total: %sus",
         Pretty(cycleStats_.postMarkUs).Str());
}

void MarkSweepCollector::ProcessFinalizers()
{
    std::function<bool(BaseObject*)> finalizable = [this](BaseObject* obj) {
        return !IsMarkedObject(obj);
    };
    collectorResources.EnqueueFinalizables(finalizable, snapshotFinalizerNum);
    collectorResources.NotifyFinalizerWork();
}

void MarkSweepCollector::PrepareCycleRef()
{
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    cycleRefWorkStack.insert(discoveredExternObjects.begin(), discoveredExternObjects.end());
    discoveredExternObjects.clear();
}

void MarkSweepCollector::PostResolveCycleTask()
{
#if defined(__OHOS__)
    if (cycleRefWorkStack.empty()) {
        return;
    }
    CJ_MRT_RolveCycleRef();
#endif
}

void MarkSweepCollector::CollectLargeGarbage()
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);

    // Tier 1: in-heap large regions — eager sweep unmarked ones
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        MSLargeSweepStats stats = centralPool->SweepLargeRegions();
        cycleStats_.losFreedCount += stats.objectCount;
        cycleStats_.losFreedRegionCount += stats.regionCount;
        cycleStats_.losFreedBytes += stats.freedBytes;
    }
}

void MarkSweepCollector::ConcurrentSweep()
{
    uint64_t phaseStart = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- ConcurrentSweep ---");

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);

    // Compute true live bytes from markBitmaps BEFORE gen flip (bitmaps are stable here).
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        cycleStats_.trueLiveBytes = centralPool->ComputeMarkedBytes();
    }

    // Mark-Sweep: generation flip now happens at the tail of MarkTermination's
    // STW, before mutators resume. ConcurrentSweep only kicks off background
    // sweeping and reclaim after the new generation is already visible.
    if (centralPool != nullptr && space.GetMSRegionManager() != nullptr) {
        // NOTE: EagerSweepBatch was removed from here because it races with the finalizer queue.
        // PostMarkTermination() enqueues finalizeable objects (dead but not yet finalized) into
        // finalizables queue. SweepRegion() would zero their memory, causing FATAL stateWord=0
        // when the finalizer thread later accesses them. BackgroundProcessor::Run() correctly
        // processes finalizers BEFORE calling BackgroundSweepLoop(). Therefore, any eager sweep
        // must happen in the background processor after finalizers are drained, not here.
        space.GetMSRegionManager()->MaybeNotifySweep();
    }

    uint64_t sweepUs = TimeUtil::MicroSeconds() - phaseStart;
    cycleStats_.concurrentSweepUs = sweepUs;
    VLOG(REPORT, "[MS-GC] ConcurrentSweep total: %sus", Pretty(sweepUs).Str());
}

// ============================================================================
// MarkNewObject: mark newly allocated objects during marking phases
// ============================================================================

void MarkSweepCollector::MarkNewObject(BaseObject* obj)
{
    Mutator* mutator = Mutator::GetMutator();
    GCPhase phase = mutator != nullptr ? mutator->GetMutatorPhase() : GetGCPhase();
    if (UNLIKELY(phase == GCPhase::GC_PHASE_MS_MARK_SETUP) ||
        UNLIKELY(phase == GCPhase::GC_PHASE_MS_CONCURRENT_MARK) ||
        UNLIKELY(phase == GCPhase::GC_PHASE_MS_MARK_TERMINATION)) {
        // Newly allocated objects may receive constructor/init stores through
        // paths where the compiler has elided a member barrier. Keep the object
        // live immediately, then scan it at MarkTermination after initialization.
        MarkObject(obj);
        PushSingleToDeferredMainThread(obj);
    }
}

// ============================================================================
// TracingCollector virtual overrides — simplified for Mark-Sweep (no tag/CAS)
// ============================================================================

void MarkSweepCollector::EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const
{
    BaseObject* target = ref.GetTargetObject();
    if (target != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(target))) {
        const_cast<MarkSweepCollector*>(this)->DiscoverAndPushObject(target, rootSet);
    }
}

void MarkSweepCollector::EnumAndTagRawRoot(ObjectRef& root, RootSet& rootSet) const
{
    // MS version: no tag/CAS, just push heap objects into root set
    BaseObject* obj = root.object;
    if (obj != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(obj))) {
        const_cast<MarkSweepCollector*>(this)->DiscoverAndPushObject(obj, rootSet);
    }
}

void MarkSweepCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack)
{
    auto refFunc = [this, obj, &workStack](RefField<>& field) {
        TraceRefField(obj, field, workStack);
    };
    obj->ForEachRefField(refFunc);
}

BaseObject* MarkSweepCollector::GetAndTryTagObj(BaseObject* obj, RefField<>& field)
{
    // No tagging in Mark-Sweep
    (void)obj;
    BaseObject* target = field.GetTargetObject();
    if (target == nullptr || !IsManagedAddress(reinterpret_cast<MAddress>(target))) {
        return nullptr;
    }

    // A managed address may still point to a reclaimed slot with cleared stateWord.
    // Treat it as invalid and do not enqueue/trace it.
    if (UNLIKELY(!target->IsValidObject())) {
        return nullptr;
    }
    return target;
}

RefField<> MarkSweepCollector::GetAndTryTagRefField(BaseObject* obj) const
{
    // No tagging in Mark-Sweep — return a RefField wrapping the object directly
    return RefField<>(obj);
}

// ============================================================================
// DFX Logging Methods (Step 5)
// ============================================================================

// Block ①: GC Entry — 2 lines
void MarkSweepCollector::LogGCEntry(uint64_t gcIndex, GCReason reason)
{
    uint32_t sweepGen = CentralRegionPool::GetGlobalSweepGen();
    VLOG(REPORT, "[MS-GC] === GC START cycle=%llu === sweepGen=%u reason=%u(%s)",
         static_cast<unsigned long long>(gcCycle_), sweepGen,
         static_cast<unsigned>(reason), g_gcRequests[reason].name);
    VLOG(REPORT, "[MS-GC] active-region bytes %s B, threshold %s B, gcIndex=%llu",
         Pretty(Heap::GetHeap().GetAllocatedSize()).Str(),
         Pretty(GetGCStats().GetThreshold()).Str(),
         static_cast<unsigned long long>(gcIndex));
}

// Snapshot previous cycle's sweep stats before PrepareForSweep resets them.
void MarkSweepCollector::SnapshotPreviousCycleSweepStats()
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        const MSSweepStats& ss = centralPool->GetSweepStats();
        cycleStats_.prevBgSweepRegions = ss.bgSweptRegions.load(std::memory_order_relaxed);
        cycleStats_.prevLazySweepRegions = ss.lazySweptRegions.load(std::memory_order_relaxed);
        cycleStats_.prevSweptSmallRegions = ss.TotalSweptRegions();
        cycleStats_.prevSweptSmallEmptiedRegions = ss.emptiedRegions.load(std::memory_order_relaxed);
        cycleStats_.prevSweptSmallRetainedRegions = ss.retainedRegions.load(std::memory_order_relaxed);
        cycleStats_.prevSweptSmallFreedBytes = ss.sweptFreedBytes.load(std::memory_order_relaxed);
        cycleStats_.prevEmptiedRegions = ss.emptiedRegions.load(std::memory_order_relaxed);
    }
    MSRegionManager* msRegionManager = space.GetMSRegionManager();
    if (msRegionManager != nullptr) {
        cycleStats_.prevReleasedTreeBytes = msRegionManager->GetReleasedTreeBytes();
        cycleStats_.prevReleasedTreeRegions = msRegionManager->GetReleasedTreeBytes() / MS_REGION_SIZE;
    }
    cycleStats_.prevSweptLargeCount = prevCycleLargeFreedCount_;
    cycleStats_.prevSweptLargeRegions = prevCycleLargeFreedRegionCount_;
    cycleStats_.prevSweptLargeFreedBytes = prevCycleLargeFreedBytes_;
}

// Block ④: Sweep Summary — output at START of next cycle with persisted stats
void MarkSweepCollector::LogSweepSummary()
{
    // Only output if this is not the first GC cycle
    if (gcCycle_ <= 1) {
        return;
    }

    size_t totalSwept = cycleStats_.prevSweptSmallRegions;
    size_t bgRegions = cycleStats_.prevBgSweepRegions;
    size_t lazyRegions = cycleStats_.prevLazySweepRegions;

    VLOG(REPORT, "[MS-GC] --- Previous-Cycle Sweep Summary (cycle=%llu) ---",
         static_cast<unsigned long long>(gcCycle_ - 1));
    VLOG(REPORT, "[MS-GC] swept small: %zu regions, freed %s B (empty=%zu -> swept cache, retained=%zu)",
         totalSwept, Pretty(cycleStats_.prevSweptSmallFreedBytes).Str(),
         cycleStats_.prevSweptSmallEmptiedRegions, cycleStats_.prevSweptSmallRetainedRegions);
    VLOG(REPORT, "[MS-GC] large reclaimed in previous cycle: %zu objects / %zu regions, freed %s B",
         cycleStats_.prevSweptLargeCount, cycleStats_.prevSweptLargeRegions,
         Pretty(cycleStats_.prevSweptLargeFreedBytes).Str());
        VLOG(REPORT, "[MS-GC] free-cache tree: releasedTree=%zu regions (%s B)",
         cycleStats_.prevReleasedTreeRegions, Pretty(cycleStats_.prevReleasedTreeBytes).Str());

    if (totalSwept > 0) {
        size_t bgPct = bgRegions * 100 / totalSwept;
        size_t lazyPct = lazyRegions * 100 / totalSwept;
        VLOG(REPORT, "[MS-GC] sweep breakdown: background %zu (%zu%%), lazy %zu (%zu%%)",
             bgRegions, bgPct, lazyRegions, lazyPct);
    } else {
        VLOG(REPORT, "[MS-GC] sweep breakdown: no regions swept");
    }
}

// Block ⑤: Region Statistics (CentralRegionPool 4-list view)
void MarkSweepCollector::DumpRegionStatsMS()
{
    uint32_t sweepGen = CentralRegionPool::GetGlobalSweepGen();
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);

    VLOG(REPORT, "[MS-GC] region statistics when gc ends (sweepGen=%u)", sweepGen);

    // RegionManager basic stats (total/active units)
    size_t totalSize = space.GetMaxCapacity();
    size_t activeRegionBytes = space.AllocatedBytes();
    size_t trueLiveBytes = cycleStats_.trueLiveBytes;
    VLOG(REPORT, "[MS-GC]   total capacity: %s B", Pretty(totalSize).Str());
    VLOG(REPORT, "[MS-GC]   active-region bytes: %s B, true live: %s B",
         Pretty(activeRegionBytes).Str(), Pretty(trueLiveBytes).Str());
    MSRegionManager* msRegionManager = space.GetMSRegionManager();
    if (msRegionManager != nullptr) {
                     cycleStats_.activeRegionCount = activeRegionBytes / MS_REGION_SIZE;
                     cycleStats_.releasedTreeRegionCount = msRegionManager->GetReleasedTreeBytes() / MS_REGION_SIZE;
                     cycleStats_.virginRegionCount = (msRegionManager->GetHeapEnd() - msRegionManager->GetInactiveZone()) /
                                                                                     MS_REGION_SIZE;
                     cycleStats_.committedRegionCount = msRegionManager->GetMaxRegionCount();
           VLOG(REPORT, "[MS-GC]   free-cache tree: releasedTree=%s B",
             Pretty(msRegionManager->GetReleasedTreeBytes()).Str());
                     VLOG(REPORT,
                                "[MS-GC]   region-accounting: active=%zu regions, releasedTree=%zu regions, virgin=%zu regions, "
                                "committed=%zu regions",
                                cycleStats_.activeRegionCount,
                                cycleStats_.releasedTreeRegionCount,
                                cycleStats_.virginRegionCount,
                                cycleStats_.committedRegionCount);
    }

    // CentralRegionPool 4-list breakdown
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->DumpPoolStats();
        uint32_t processorNum = Runtime::CurrentRef() != nullptr
            ? Runtime::Current().GetConcurrencyModel().GetProcessorNum()
            : 0;
        VLOG(REPORT, "[MS-GC]   scheduler P: %u, tl-active regions: %zu, alloc buffers: %zu",
             processorNum, centralPool->CountTLActiveRegions(), centralPool->GetAllocBufferCount());
    }
}

// Block ⑥: GC Summary — 3 lines
void MarkSweepCollector::LogGCSummary(uint64_t gcTimeUs)
{
    size_t trueLive = cycleStats_.trueLiveBytes;
    size_t activeRegionBytes = Heap::GetHeap().GetAllocatedSize();
    double utilization = (activeRegionBytes == 0) ? 0 : (static_cast<double>(trueLive) / activeRegionBytes) * 100;
    double activeRegionSlack = 100.0 - utilization;
    uint64_t accountedUs = cycleStats_.preEnumRootsUs + cycleStats_.markSetupUs +
                           cycleStats_.concurrentMarkUs + cycleStats_.markTerminationUs +
                           cycleStats_.postMarkUs + cycleStats_.concurrentSweepUs +
                           cycleStats_.postGcHookUs;
    uint64_t otherUs = (gcTimeUs > accountedUs) ? (gcTimeUs - accountedUs) : 0;

    VLOG(REPORT,
         "[MS-GC] active-region bytes: %s, true live: %s B, true-live utilization: %.2f%%, "
         "slack: %.2f%%, marked %zu objects",
         Pretty(activeRegionBytes).Str(), Pretty(trueLive).Str(), utilization, activeRegionSlack,
         markedObjectCount.load(std::memory_order_relaxed));
    VLOG(REPORT,
         "[MS-GC] total gc time: %s us (PreEnum %s + EnumRoots %s + ConcMark %s + "
         "MarkTerm %s + PostMark %s + ConcSweep %s + PostGC %s + Other %s)",
         Pretty(gcTimeUs).Str(),
         Pretty(cycleStats_.preEnumRootsUs).Str(),
         Pretty(cycleStats_.markSetupUs).Str(),
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         Pretty(cycleStats_.markTerminationUs).Str(),
         Pretty(cycleStats_.postMarkUs).Str(),
         Pretty(cycleStats_.concurrentSweepUs).Str(),
         Pretty(cycleStats_.postGcHookUs).Str(),
         Pretty(otherUs).Str());

    // Also output the standard LOG(RTLOG_INFO) summary for compatibility
    LOG(RTLOG_INFO,
        "[MS-GC] GC cycle=%llu: marked %zu objects, %.2f%% true-live utilization (%s/%s), gc time: %sus",
        static_cast<unsigned long long>(gcCycle_),
        markedObjectCount.load(std::memory_order_relaxed),
        utilization,
        Pretty(trueLive).Str(), Pretty(activeRegionBytes).Str(),
        Pretty(gcTimeUs).Str());
}

// ============================================================================
// MS-GC specific UpdateGCStats: simplified Go-style threshold
// ============================================================================
void MarkSweepCollector::UpdateGCStats()
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    GCStats& gcStats = GetGCStats();

    size_t oldThreshold = gcStats.heapThreshold;
    // Use true live bytes (computed from markBitmaps) instead of AllocatedBytes()
    // which returns totalMSRegionBytes_ (committed region bytes, only increases).
    size_t liveBytes = cycleStats_.trueLiveBytes;
    size_t committedBytes = space.AllocatedBytes();
    size_t heapSize = space.GetMaxCapacity();
    size_t recentBytes = space.GetRecentAllocatedSize();

    // 2 / 3: when committed bytes is over 2/3 heap size, the async allocation need to be closed.
    if (committedBytes > static_cast<size_t>(heapSize * GetMsAsyncAllocDisableRatio())) {
        space.EnableAsyncAllocation(false);
    } else {
        space.EnableAsyncAllocation(true);
    }

    // Step9: drive the next cycle primarily from true-live bytes instead of
    // blending active-region slack into the threshold base.
    double heapGrowth = 1 + (CangjieRuntime::GetHeapParam().heapGrowth);
    size_t liveDrivenThreshold = static_cast<size_t>(liveBytes * heapGrowth);

    // Keep a minimum post-GC headroom so tiny live sets do not cause immediate
    // re-triggering after a few small region refills.
    size_t minHeadroom = std::max(static_cast<size_t>(heapSize * GetMsMinHeadroomHeapRatio()),
                                  static_cast<size_t>(MS_REGION_SIZE * GetMsMinHeadroomRegions()));
    size_t newThreshold = std::max(liveDrivenThreshold, liveBytes + minHeadroom);

    // 0.98: make sure new threshold does not exceed reasonable limit.
    newThreshold = std::min(newThreshold, static_cast<size_t>(heapSize * GetMsThresholdHeapCapRatio()));
    gcStats.heapThreshold = std::min(newThreshold, CangjieRuntime::GetGCParam().gcThreshold);
    // Persist the post-GC baseline used by MSGC's effective-allocation trigger:
    // effectiveAlloc = lastTrueLiveBytes + max(0, currentAllocated - baselineAllocatedBytes).
    gcStats.lastTrueLiveBytes = liveBytes;
    gcStats.baselineAllocatedBytes = committedBytes;
    size_t survivedBytes = (liveBytes > recentBytes) ? (liveBytes - recentBytes) : 0;
    VLOG(REPORT, "[MS-GC] --- Total ---");
    VLOG(REPORT,
            "[MS-GC] live bytes %zu (survived %zu, recent-allocated %zu), active-region bytes %zu, "
            "live-driven threshold base %zu, min headroom %zu, update gc threshold %zu -> %zu",
         liveBytes, survivedBytes, recentBytes, committedBytes,
            liveBytes, minHeadroom, oldThreshold, gcStats.heapThreshold);
    VLOG(REPORT, "[MS-GC]   active-region bytes %s B, true live %s B (%.1f%% utilization)",
         Pretty(committedBytes).Str(), Pretty(liveBytes).Str(),
         (committedBytes == 0) ? 0.0 : (static_cast<double>(liveBytes) / committedBytes) * 100);
    VLOG(REPORT, "[MS-GC]   trigger baseline persisted: lastTrueLive=%zu baselineAllocated=%zu",
         gcStats.lastTrueLiveBytes, gcStats.baselineAllocatedBytes);
    TRACE_COUNT("CJRT_post_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
