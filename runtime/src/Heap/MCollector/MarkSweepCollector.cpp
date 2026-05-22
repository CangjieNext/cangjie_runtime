// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MarkSweepCollector.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "CangjieRuntime.h"
#include "Common/GCRefFieldScan.inline.h"
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

bool IsMSMarkingFastState(uint8_t gcFastState)
{
    return (gcFastState & ThreadLocal::GC_FAST_STATE_MS_MARKING) != 0;
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

static inline bool IsTraceableObjectAddress(BaseObject* obj)
{
    if (obj == nullptr) {
        return false;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (!IsManagedAddress(addr)) {
        return false;
    }

    MSRegionInfo* msInfo = MSRegionInfoArray::GetRegionInfoValidated(addr);
    if (msInfo == nullptr) {
        return false;
    }

    if (msInfo->regionType == MSRegionType::MS_REGION_LARGE) {
        MSRegionInfo* base = msInfo;
        if (base->regionEnd == 0) {
            base = MSRegionInfoArray::GetRegionInfoByIdx(static_cast<size_t>(msInfo->reserved));
        }
        return base->allocPtr == addr && (addr + sizeof(uintptr_t) <= base->regionEnd);
    }

    if (msInfo->regionType == MSRegionType::MS_REGION_FREE ||
        msInfo->regionType == MSRegionType::MS_REGION_EMPTY_CACHE ||
        msInfo->regionType == MSRegionType::MS_REGION_SPAN) {
        return false;
    }

    uintptr_t regionStart = msInfo->RegionStart();
    size_t slotSize = msInfo->GetSlotSize();
    if (slotSize == 0) {
        return false;
    }
    if (addr < regionStart || addr + sizeof(uintptr_t) > msInfo->regionEnd) {
        return false;
    }
    if (((addr - regionStart) % slotSize) != 0) {
        return false;
    }
    if (msInfo->regionType == MSRegionType::MS_REGION_BUMP && addr >= msInfo->allocPtr) {
        return false;
    }

    return true;
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
    MSAllocBuffer_DumpAndResetSlowAllocCounters();
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
    DumpGCStateSnapshot("preGCMark");
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
//   markBitmaps, then stamp/reclassify any residual unswept regions as swept,
//   and freeze this cycle's sweep snapshot. Regions returned by mutators after
//   this point are stamped into the new sweep generation and will be swept in
//   the next GC cycle.
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
    // → reclassify residual unswept entries → freeze this cycle's sweep
    // snapshot. Lazy sweep stays disabled until MarkTermination completes,
    // because the mark bitmap is not yet final.
    // Safe pre-STW: no SweepRegion running, mark-at-alloc inactive.
    if (centralPool != nullptr) {
        centralPool->StopAndWaitForAllSweep();
        centralPool->ClearAllMarkBitmaps();
        centralPool->ReclassifyUnsweptToSwept();
        centralPool->PrepareForSweep();
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
        EnumAndMarkRoots(markWorkStack_.UnsafeRaw());
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
    if (UNLIKELY(!IsTraceableObjectAddress(obj) || !obj->IsValidObject())) {
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

// Assist ratio escalation coefficient (default 0.1, configurable via CJ_MS_ASSIST_RATIO_COEFF).
// Each loop iteration in RunConcurrentMarkLoop, the ratio is multiplied by (1 + coeff),
// up to a maximum of 10x the initial value.
static double GetAssistRatioCoeff()
{
    static double coeff = []() -> double {
        const char* env = std::getenv("CJ_MS_ASSIST_RATIO_COEFF");
        if (env != nullptr && env[0] != '\0') {
            double v = std::strtod(env, nullptr);
            if (v > 0.0 && v <= 10.0) {
                return v;
            }
        }
        return 0.1;
    }();
    return coeff;
}

uint64_t ComputeAssistRatioCap(uint64_t initialRatioScaled)
{
    long double capped = std::ceil(static_cast<long double>(initialRatioScaled) *
                                   static_cast<long double>(MarkSweepCollector::GetAssistRatioMaxMultiplier()));
    if (capped < 1.0L) {
        return 1;
    }
    if (capped >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(capped);
}

uint64_t ComputeNextAssistRatioScaled(uint64_t currentRatioScaled, uint64_t ratioCapScaled)
{
    if (currentRatioScaled >= ratioCapScaled) {
        return ratioCapScaled;
    }

    long double escalated = std::ceil(static_cast<long double>(currentRatioScaled) *
                                      (1.0L + static_cast<long double>(GetAssistRatioCoeff())));
    if (escalated >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return ratioCapScaled;
    }

    uint64_t nextRatioScaled = static_cast<uint64_t>(escalated);
    if (nextRatioScaled <= currentRatioScaled) {
        nextRatioScaled = currentRatioScaled + 1;
    }
    return std::min(nextRatioScaled, ratioCapScaled);
}

// Fixed-point scale denominator for assist ratio (configurable via CJ_MS_ASSIST_RATIO_SCALE).
uint64_t MarkSweepCollector::GetAssistRatioScale()
{
    static uint64_t scale = []() -> uint64_t {
        return static_cast<uint64_t>(ReadEnvCount("CJ_MS_ASSIST_RATIO_SCALE", 1024));
    }();
    return scale;
}

// Per-assist-call object budget (configurable via CJ_MS_ASSIST_DEBT_TRIGGER).
size_t MarkSweepCollector::GetAssistDebtTriggerObjects() const
{
    static size_t trigger = []() -> size_t {
        return ReadEnvCount("CJ_MS_ASSIST_DEBT_TRIGGER", 256);
    }();
    return trigger;
}

// Maximum ratio escalation multiplier over the initial value (configurable via
// CJ_MS_ASSIST_RATIO_MAX_MULT, default 10).
// A value of 1.5 caps the ratio at 1.5x the initial, giving moderate assist in
// long GC cycles (where the ratio escalates fully) while leaving short GC cycles
// effectively unassisted (too few loop iterations to reach the cap).
double MarkSweepCollector::GetAssistRatioMaxMultiplier()
{
    static double mult = []() -> double {
        const char* env = std::getenv("CJ_MS_ASSIST_RATIO_MAX_MULT");
        if (env != nullptr && env[0] != '\0') {
            double v = std::strtod(env, nullptr);
            if (v > 0.0 && v <= 100.0) {
                return v;
            }
        }
        return 10.0;
    }();
    return mult;
}

void MarkSweepCollector::ConcurrentMark()
{
    // activeCollector_ was already set in MarkSetup (before STW).

    // 8. Transition to concurrent marking phase
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_CONCURRENT_MARK, true);

    // 9. Reset assist stats and bgScanCredit
    assistCallTotal_.store(0, std::memory_order_relaxed);
    assistProcessedCount_.store(0, std::memory_order_relaxed);
    assistFastSkipCount_.store(0, std::memory_order_relaxed);
    globalShareCount_.store(0, std::memory_order_relaxed);
    bgScanCredit_.store(0, std::memory_order_relaxed);
    bgScanCreditConsumed_.store(0, std::memory_order_relaxed);

    // 10. Init per-P GCWork for all existing mutators before enabling assist.
    //     New mutators that join later will lazily init in MSMarkAssist.
    InitAllPerPGCWork();

    // 11. Enable assist: compute initial ratio from previous cycle stats.
    {
        uint64_t ratioScale = GetAssistRatioScale();
        size_t prevMarked = prevCycleMarkedObjects_.load(std::memory_order_relaxed);
        size_t allocSize = Heap::GetHeap().GetAllocatedSize();
        uint64_t initialRatioScaled = 1;  // minimum non-zero
        if (prevMarked > 0 && allocSize > 0) {
            // objects-per-byte * scale; start conservatively at 1/4 of computed value
            uint64_t computed = static_cast<uint64_t>(prevMarked) * ratioScale / allocSize;
            initialRatioScaled = std::max((uint64_t)1, computed / 4);
           }
           assistWorkPerByteScaled_.store(initialRatioScaled, std::memory_order_release);
           VLOG(REPORT,
               "[MS-GC] Assist enabled: ratioScaled=%llu coeff=%.3f maxMult=%.3f (prevMarked=%zu, allocSize=%zu)",
               (unsigned long long)initialRatioScaled,
               GetAssistRatioCoeff(),
               GetAssistRatioMaxMultiplier(),
               prevMarked,
               allocSize);
    }

    // 12. Run concurrent marking loop using gcThreadWork_ + globalFull_.
    RunConcurrentMarkLoop();

    // 11. Process WeakRefs deferred during concurrent mark, before entering STW.
    ProcessDeferredWeakRefsPreSTW();

    // 12. Stop assist by clearing ratio.
    assistWorkPerByteScaled_.store(0, std::memory_order_release);
}

void MarkSweepCollector::RunConcurrentMarkLoop()
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- ConcurrentMark (gcThreadWork_ + globalFull_ LFStack) ---");

    // Init GC thread's own PerPGCWork (sets globalFull_ pointer for TryGetObj refill).
    // Then pack all root objects into WorkBufs and push directly to globalFull_.
    // GC thread and assist threads both compete for work from globalFull_ via TryGetObj().
    gcThreadWork_.Init(&globalFull_, &workBufPool_);
    TransferRootsToGCWork();

    convergenceStableRounds_ = 0;
    size_t localCreditAccum = 0;
    const uint64_t initialRatioScaled = assistWorkPerByteScaled_.load(std::memory_order_acquire);
    const uint64_t ratioCapScaled = ComputeAssistRatioCap(initialRatioScaled);

    while (true) {
        // Drain a slice via gcThreadWork_ (refills from globalFull_ on empty).
        size_t scannedCount = 0;
        DrainGCWork(gcThreadWork_, kSliceMaxObjects, scannedCount);

        uint64_t currentRatioScaled = assistWorkPerByteScaled_.load(std::memory_order_relaxed);
        if (currentRatioScaled != 0 && currentRatioScaled < ratioCapScaled) {
            assistWorkPerByteScaled_.store(ComputeNextAssistRatioScaled(currentRatioScaled, ratioCapScaled),
                                           std::memory_order_release);
        }

        localCreditAccum += scannedCount;
        if (localCreditAccum >= kCreditFlushThreshold) {
            bgScanCredit_.fetch_add(static_cast<int64_t>(localCreditAccum), std::memory_order_release);
            localCreditAccum = 0;
        }

        if (gcThreadWork_.Empty() && globalFull_.Empty()) {
            if (CheckConvergenceAndBreak(localCreditAccum)) {
                break;
            }
        } else {
            convergenceStableRounds_ = 0;
        }
    }

    // Snapshot assist stats
    cycleStats_.assistCallCount = assistCallTotal_.load(std::memory_order_relaxed);
    cycleStats_.assistProcessedObjects = assistProcessedCount_.load(std::memory_order_relaxed);
    cycleStats_.assistFastSkipCount = assistFastSkipCount_.load(std::memory_order_relaxed);
    cycleStats_.globalShareCount = globalShareCount_.load(std::memory_order_relaxed);
    cycleStats_.bgScanCreditConsumed = bgScanCreditConsumed_.load(std::memory_order_relaxed);

    cycleStats_.concurrentMarkUs = TimeUtil::MicroSeconds() - t0;
    VLOG(REPORT,
         "[MS-GC] ConcurrentMark time: %sus, marked %zu objects "
            "(assist: %zu calls, %zu processed, %zu bgCredit-skip; "
            "LFStack: push %zu, bgCredit consumed %zu, final ratioScaled=%llu)",
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         markedObjectCount.load(std::memory_order_relaxed),
         cycleStats_.assistCallCount,
         cycleStats_.assistProcessedObjects,
         cycleStats_.assistFastSkipCount,
         cycleStats_.globalShareCount,
            cycleStats_.bgScanCreditConsumed,
            (unsigned long long)assistWorkPerByteScaled_.load(std::memory_order_relaxed));
}

// ============================================================================
// CheckConvergenceAndBreak: convergence gate for RunConcurrentMarkLoop.
//
// Called each time markWorkStack_ is empty.
// Returns true when true convergence is confirmed and the loop should exit.
// ============================================================================

bool MarkSweepCollector::CheckConvergenceAndBreak(size_t& localCreditAccum)
{
    convergenceStableRounds_++;
    if (convergenceStableRounds_ < kConvergenceRequiredRounds) {
        // Short yield — let in-flight write-barrier flushes arrive in globalFull_.
        usleep(kConvergenceYieldUs);
        return false;
    }

    usleep(kConvergenceFlushWaitUs);

    // After longer wait, check if new barrier overflow arrived in globalFull_.
    if (!gcThreadWork_.Empty() || !globalFull_.Empty()) {
        VLOG(REPORT, "[MS-GC] Pre-convergence flush: residual work found, re-draining");
        convergenceStableRounds_ = 0;
        return false;
    }

    // True convergence: flush remaining bgScanCredit and signal loop exit.
    if (localCreditAccum > 0) {
        bgScanCredit_.fetch_add(static_cast<int64_t>(localCreditAccum), std::memory_order_release);
        localCreditAccum = 0;
    }
    return true;
}

void MarkSweepCollector::DrainConcurrentOverflowQueueToMarkWork()
{
    std::lock_guard<std::mutex> lock(concurrentOverflowQueueMutex_);
    markWorkStack_.insert(concurrentOverflowQueue_);
}

void MarkSweepCollector::DrainMarkWorkStack(size_t maxObjects, size_t& scannedCount)
{
    scannedCount = 0;
    for (size_t i = 0; i < maxObjects; i++) {
        if (markWorkStack_.empty()) {
            break;
        }

        BaseObject* obj = markWorkStack_.back();
        markWorkStack_.pop_back();
        if (UNLIKELY(!IsTraceableObjectAddress(obj))) {
            VLOG(REPORT, "[MS-GC] DrainMarkWorkStack: drop invalid obj=%p", static_cast<void*>(obj));
            continue;
        }

        if (UNLIKELY(!obj->IsValidObject())) {
            LOG(RTLOG_FATAL, "[MS-GC] DrainMarkWorkStack: FATAL invalid stateWord obj=%p, "
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
            PushSingleToDeferredMainThread(obj);
            continue;
        }

        TraceObjectRefFields(obj, markWorkStack_.UnsafeRaw());
    }
}

// ============================================================================
// ProcessDeferredWeakRefsPreSTW: handle WeakRefs deferred by assist threads
// during concurrent mark, on the GC thread, non-STW.
// ============================================================================

void MarkSweepCollector::ProcessDeferredWeakRefsPreSTW()
{
    // Drain items pushed by PushSingleToDeferredMainThread (from assist threads).
    // Assists are now disabled (ratio = 0), so no concurrent writers.
    GreyWorkQueue localQueue;
    {
        std::lock_guard<std::mutex> lock(deferredMainThreadMutex_);
        localQueue.insert(deferredMainThreadQueue_);
    }
    if (localQueue.empty()) {
        return;
    }

    size_t processed = 0;
    while (!localQueue.empty()) {
        BaseObject* obj = localQueue.back();
        localQueue.pop_back();
        if (obj == nullptr || !obj->HasRefField()) {
            continue;
        }
        if (UNLIKELY(obj->IsWeakRef())) {
            RefField<>* referentField = reinterpret_cast<RefField<>*>(
                reinterpret_cast<uintptr_t>(obj) + sizeof(uintptr_t));
            BaseObject* referent = GetAndTryTagObj(obj, *referentField);
            if (referent != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(referent))) {
                WeakRefBuffer::Instance().Insert(obj);
            }
            processed++;
            continue;
        }
        // Non-WeakRef deferred items (defensive): trace normally.
        TraceObjectRefFields(obj, markWorkStack_.UnsafeRaw());
        processed++;
    }
    while (!markWorkStack_.empty()) {
        size_t scannedCount = 0;
        DrainMarkWorkStack(kSliceMaxObjects, scannedCount);
        if (scannedCount == 0) {
            break;
        }
    }
    if (processed > 0) {
        VLOG(REPORT, "[MS-GC] ProcessDeferredWeakRefsPreSTW: %zu deferred items processed", processed);
    }
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

        if (UNLIKELY(!IsTraceableObjectAddress(obj))) {
            VLOG(REPORT, "[MS-GC] DrainGCWork: drop invalid obj=%p", static_cast<void*>(obj));
            continue;
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
            // Defer WeakRef to GC main thread — safe from both GC thread and mutator assist.
            PushSingleToDeferredMainThread(obj);
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
    ForEachRefFieldGCInline(obj, refFunc);
}

// ============================================================================
// TransferRootsToGCWork: pack root objects into WorkBufs and push directly to
// globalFull_, same as WriteBarrierBuffer overflow does.
// GC thread and assist threads both consume from globalFull_ via TryGetObj().
// Called once at start of ConcurrentMark after root enumeration.
// ============================================================================
void MarkSweepCollector::TransferRootsToGCWork()
{
    if (markWorkStack_.empty()) {
        VLOG(REPORT, "[MS-GC] TransferRootsToGCWork: 0 root objects → globalFull_");
        return;
    }

    size_t transferred = 0;
    WorkBuf* buf = workBufPool_.Get();
    while (!markWorkStack_.empty()) {
        BaseObject* obj = markWorkStack_.back();
        markWorkStack_.pop_back();
        if (buf->Full()) {
            globalFull_.Push(buf);
            buf = workBufPool_.Get();
        }
        buf->Push(obj);
        transferred++;
    }
    // Push the last (possibly partial) buf
    if (!buf->Empty()) {
        globalFull_.Push(buf);
    } else {
        workBufPool_.Put(buf);
    }

    VLOG(REPORT, "[MS-GC] TransferRootsToGCWork: %zu root objects → globalFull_", transferred);
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
    size_t n = wbBuf.Count();
    if (n == 0) {
        return;
    }
    BaseObject** data = wbBuf.Data();
    // Route barrier overflow directly to globalFull_ via WorkBufs so that:
    //   - gcThreadWork_ (GC thread) sees them via TryGetObj() refill
    //   - per-mutator PerPGCWork (assist) sees them via TryGetObj() refill
    // Objects in wbBuf are already marked (DiscoverObject was called in EnqueueRef).
    WorkBuf* buf = workBufPool_.Get();
    size_t bufsUsed = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] == nullptr) continue;
        if (buf->Full()) {
            globalFull_.Push(buf);
            bufsUsed++;
            buf = workBufPool_.Get();
        }
        buf->Push(data[i]);
    }
    if (!buf->Empty()) {
        globalFull_.Push(buf);
        bufsUsed++;
    } else {
        workBufPool_.Put(buf);
    }
    wbBuf.Clear();
    if (bufsUsed > 0) {
        globalShareCount_.fetch_add(bufsUsed, std::memory_order_relaxed);
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
// ============================================================================

size_t MarkSweepCollector::MSMarkAssist(size_t debtObjects)
{
    if (debtObjects == 0) {
        return 0;
    }

    assistCallTotal_.fetch_add(1, std::memory_order_relaxed);

    // Step5: Try to steal bgScanCredit first — if GC thread is ahead, skip/partially skip work.
    size_t paidByCredit = TryStealBgCreditPartial(debtObjects);
    if (paidByCredit >= debtObjects) {
        assistFastSkipCount_.fetch_add(1, std::memory_order_relaxed);
        return debtObjects;
    }
    size_t assistBudget = std::min(GetAssistDebtTriggerObjects(), debtObjects - paidByCredit);
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
    DrainGCWork(gcw, assistBudget, nProcessed);

    assistProcessedCount_.fetch_add(nProcessed, std::memory_order_relaxed);
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

    // Step5: Reset all per-P gcWork and return WorkBufs to pool.
    ResetAllPerPGCWork();

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

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->ReenableLazySweep();
    }
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
    VLOG(REPORT, "[MS-GC] --- PostMarkTermination ---");
    uint64_t phaseStart = TimeUtil::MicroSeconds();

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
        size_t totalUnswept = centralPool->GetTotalUnsweptCount();
        // Kick off an eager sweep batch on the GC thread; the stop condition is
        // the eager-sweep deadline, not a half-unswept cap.
        size_t eagerSweepTarget = totalUnswept;
        size_t emptyBytesBefore = centralPool->GetEmptySweptBytes();
        cycleStats_.eagerSweepBatchRegions = centralPool->EagerSweepBatch(eagerSweepTarget);
        size_t emptyBytesAfter = centralPool->GetEmptySweptBytes();
        size_t eagerSweepEmptyBytes =
            (emptyBytesAfter > emptyBytesBefore) ? (emptyBytesAfter - emptyBytesBefore) : 0;
        cycleStats_.eagerSweepEmptiedRegions = eagerSweepEmptyBytes / MS_REGION_SIZE;
        cycleStats_.eagerSweepTotalUnswept = totalUnswept;
        VLOG(REPORT,
             "[MS-GC] ConcurrentSweep eager: swept=%zu regions, unsweptBefore=%zu",
             cycleStats_.eagerSweepBatchRegions, totalUnswept);
    }

    // Mark-Sweep: generation flip now happens at the tail of MarkTermination's
    // STW, before mutators resume. ConcurrentSweep only kicks off background
    // sweeping and reclaim after the new generation is already visible.
    uint64_t sweepUs = TimeUtil::MicroSeconds() - phaseStart;
    cycleStats_.concurrentSweepUs = sweepUs;
    VLOG(REPORT, "[MS-GC] ConcurrentSweep total: %sus", Pretty(sweepUs).Str());
}

// ============================================================================
// MarkNewObject: mark newly allocated objects during marking phases
// ============================================================================

void MarkSweepCollector::MarkNewObject(BaseObject* obj)
{
    GCPhase phase = GetGCPhase();
    if (LIKELY(phase != GCPhase::GC_PHASE_MS_MARK_SETUP) &&
        LIKELY(phase != GCPhase::GC_PHASE_MS_CONCURRENT_MARK) &&
        LIKELY(phase != GCPhase::GC_PHASE_MS_MARK_TERMINATION)) {
        return;
    }

    MarkObject(obj);
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
    ForEachRefFieldGCInline(obj, refFunc);
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

// Block ①: GC Entry — 3 lines
void MarkSweepCollector::LogGCEntry(uint64_t gcIndex, GCReason reason)
{
    uint32_t sweepGen = CentralRegionPool::GetGlobalSweepGen();
    VLOG(REPORT, "[MS-GC] === GC START cycle=%llu === sweepGen=%u reason=%u(%s)",
         static_cast<unsigned long long>(gcCycle_), sweepGen,
         static_cast<unsigned>(reason), g_gcRequests[reason].name);
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    VLOG(REPORT, "[MS-GC] active-region bytes %s B, threshold %s B, gcIndex=%llu",
        Pretty(space.GetHeapInUseBytes()).Str(),
         Pretty(GetGCStats().GetThreshold()).Str(),
         static_cast<unsigned long long>(gcIndex));
    CentralRegionPool* centralPool = space.GetCentralPool();
    MSRegionManager* msRegionManager = space.GetMSRegionManager();
    size_t unsweptCount = (centralPool != nullptr) ? centralPool->GetTotalUnsweptCount() : 0;
    size_t emptyCount = (centralPool != nullptr) ? centralPool->GetEmptySweptRegionCount() : 0;
    size_t hotFreeBytes = (msRegionManager != nullptr) ? msRegionManager->GetHotFreeBytes() : 0;
    size_t hotFreeCount = hotFreeBytes / MS_REGION_SIZE;
    VLOG(REPORT, "[MS-GC] unswept: %zu regions, emptyCache: %zu regions, hotFree: %zu regions",
        unsweptCount, emptyCount, hotFreeCount);
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
        VLOG(REPORT, "[MS-GC] swept small: %zu regions, empty=%zu, bg=%zu lazy=%zu",
            totalSwept, cycleStats_.prevSweptSmallEmptiedRegions, bgRegions, lazyRegions);
    VLOG(REPORT, "[MS-GC] large reclaimed in previous cycle: %zu objects / %zu regions, freed %s B",
         cycleStats_.prevSweptLargeCount, cycleStats_.prevSweptLargeRegions,
         Pretty(cycleStats_.prevSweptLargeFreedBytes).Str());
}

// Block ⑤: Region Statistics (CentralRegionPool 4-list view)
void MarkSweepCollector::DumpRegionStatsMS()
{
    uint32_t sweepGen = CentralRegionPool::GetGlobalSweepGen();
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);

    VLOG(REPORT, "[MS-GC] --- Total ---");
    VLOG(REPORT, "[MS-GC] region statistics when gc ends (sweepGen=%u)", sweepGen);

    // RegionManager basic stats (total/active units)
    size_t totalSize = space.GetMaxCapacity();
    size_t activeRegionBytes = space.GetHeapInUseBytes();
    size_t trueLiveBytes = cycleStats_.trueLiveBytes;
    double utilization = (activeRegionBytes == 0)
        ? 0.0
        : (static_cast<double>(trueLiveBytes) / static_cast<double>(activeRegionBytes)) * 100.0;
    MSRegionManager* msRegionManager = space.GetMSRegionManager();
    size_t currentHeapSize = (msRegionManager != nullptr) ? msRegionManager->GetCurrentCapacityBytes() : 0;
    VLOG(REPORT, "[MS-GC]   total capacity: %s B, current heap: %s B",
         Pretty(totalSize).Str(), Pretty(currentHeapSize).Str());
    VLOG(REPORT, "[MS-GC]   active-region bytes: %s B, true live: %s B, true-live utilization: %.2f%%",
         Pretty(activeRegionBytes).Str(), Pretty(trueLiveBytes).Str(), utilization);
    if (msRegionManager != nullptr) {
        cycleStats_.activeRegionCount = activeRegionBytes / MS_REGION_SIZE;
        size_t emptyRegionCount = 0;
        if (CentralRegionPool* centralPool = space.GetCentralPool(); centralPool != nullptr) {
            emptyRegionCount = centralPool->GetEmptySweptRegionCount();
        }
        size_t hotFreeRegionCount = msRegionManager->GetHotFreeBytes() / MS_REGION_SIZE;
        VLOG(REPORT,
             "[MS-GC]   region-accounting: active=%zu regions, empty=%zu regions, hotFree=%zu regions",
             cycleStats_.activeRegionCount,
             emptyRegionCount,
             hotFreeRegionCount);
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

void MarkSweepCollector::DumpGCStateSnapshot(const char* stage)
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool == nullptr) {
        VLOG(REPORT, "[MS-GC] --- %s --- (no central pool)", stage);
        return;
    }

    size_t allocBufferCount = centralPool->GetAllocBufferCount();
    size_t bufferRegionCount = centralPool->GetAllAllocBufferRegionCount();
    size_t bufferActiveRegionCount = centralPool->GetAllAllocBufferActiveRegionCount();
    size_t bufferCachedRegionCount = centralPool->GetAllAllocBufferCachedRegionCount();
    size_t emptyCacheCount = centralPool->GetEmptySweptRegionCount();
    size_t largeRegionCount = centralPool->GetLargeRegionCount();

    VLOG(REPORT, "[MS-GC] --- %s ---", stage);
    VLOG(REPORT, "[MS-GC]   alloc buffers: %zu", allocBufferCount);
    VLOG(REPORT, "[MS-GC]   alloc-buffer regions: total=%zu active=%zu cached=%zu",
         bufferRegionCount, bufferActiveRegionCount, bufferCachedRegionCount);
    VLOG(REPORT, "[MS-GC]   central pool: empty=%zu large=%zu", emptyCacheCount, largeRegionCount);

    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        CentralRegionPool::BucketPressure pressure = centralPool->GetBucketPressure(cls);
        size_t clsTotal = pressure.partialSwept + pressure.partialUnswept + pressure.fullSwept + pressure.fullUnswept;
        if (clsTotal == 0) {
            continue;
        }
        VLOG(REPORT,
             "[MS-GC]     cls=%u partialSwept=%zu fullSwept=%zu partialUnswept=%zu fullUnswept=%zu total=%zu",
             static_cast<unsigned>(cls),
             pressure.partialSwept, pressure.fullSwept,
             pressure.partialUnswept, pressure.fullUnswept, clsTotal);
    }
}

// Block ⑥: GC Summary — 3 lines
void MarkSweepCollector::LogGCSummary(uint64_t gcTimeUs)
{
    uint64_t accountedUs = cycleStats_.preEnumRootsUs + cycleStats_.markSetupUs +
                           cycleStats_.concurrentMarkUs + cycleStats_.markTerminationUs +
                           cycleStats_.postMarkUs + cycleStats_.concurrentSweepUs +
                           cycleStats_.postGcHookUs;
    uint64_t otherUs = (gcTimeUs > accountedUs) ? (gcTimeUs - accountedUs) : 0;

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
}

// ============================================================================
// MS-GC specific UpdateGCStats: simplified Go-style threshold
// ============================================================================
void MarkSweepCollector::UpdateGCStats()
{
    VLOG(REPORT, "[MS-GC] --- UpdateGCStats ---");
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    GCStats& gcStats = GetGCStats();

    size_t oldThreshold = gcStats.GetThreshold();
    size_t oldLastTrueLiveBytes = gcStats.lastTrueLiveBytes;
    size_t preActiveRegionBytes = gcStats.lastRegionBytes;
    size_t liveBytes = cycleStats_.trueLiveBytes;
    size_t activeRegionBytes = space.GetHeapInUseBytes();

    gcStats.lastTrueLiveBytes = liveBytes;
    gcStats.lastRegionBytes = activeRegionBytes;
    gcStats.baselineAllocatedBytes = activeRegionBytes;

    size_t estimatedLiveBytes = activeRegionBytes;
    {
        size_t eagerSwept = cycleStats_.eagerSweepBatchRegions;
        size_t eagerEmptied = cycleStats_.eagerSweepEmptiedRegions;
        size_t totalUnswept = cycleStats_.eagerSweepTotalUnswept;
        if (eagerSwept > 0 && totalUnswept > 0 && activeRegionBytes > 0) {
            double emptyRate = static_cast<double>(eagerEmptied) / static_cast<double>(eagerSwept);
            if (emptyRate > 1.0) {
                emptyRate = 1.0;
            }
            size_t estimatedTotalEmpty = static_cast<size_t>(static_cast<double>(totalUnswept) * emptyRate);
            size_t estimatedEmptyBytes = estimatedTotalEmpty * MS_REGION_SIZE;
            estimatedLiveBytes = (activeRegionBytes > estimatedEmptyBytes)
                ? (activeRegionBytes - estimatedEmptyBytes)
                : 0;
            if (estimatedLiveBytes < liveBytes) {
                estimatedLiveBytes = liveBytes;
            }
            VLOG(REPORT,
                 "[MS-GC] LiveEstimate: eagerSwept=%zu emptied=%zu totalUnswept=%zu "
                 "emptyRate=%.2f estimatedEmpty=%zu estimatedLiveBytes=%zuB",
                 eagerSwept, eagerEmptied, totalUnswept,
                 emptyRate, estimatedTotalEmpty, estimatedLiveBytes);
        }
    }

    if (estimatedLiveBytes > 0) {
        size_t threshold = gcStats.UpdateHeapThreshold(estimatedLiveBytes);
        VLOG(REPORT,
             "[MS-GC] heapThreshold: oldThreshold=%zuB newThreshold=%zuB "
                         "prevLive=%zuB curLive=%zuB preActiveRegionBytes=%zuB activeRegion=%zuB estimatedLive=%zuB",
               oldThreshold, threshold, oldLastTrueLiveBytes, liveBytes, preActiveRegionBytes, activeRegionBytes,
             estimatedLiveBytes);
    } else if (activeRegionBytes > 0) {
        size_t threshold = gcStats.UpdateHeapThreshold(activeRegionBytes);
        VLOG(REPORT,
             "[MS-GC] heapThreshold (fallback): oldThreshold=%zuB newThreshold=%zuB "
                         "preActiveRegionBytes=%zuB activeRegion=%zuB",
               oldThreshold, threshold, preActiveRegionBytes, activeRegionBytes);
    }

    VLOG(REPORT,
         "[MS-GC] live bytes %zu, activeRegion %zu, update gc threshold %zu -> %zu",
         liveBytes, activeRegionBytes, oldThreshold, gcStats.GetThreshold());
    TRACE_COUNT("CJRT_post_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
