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
#include "Heap/MAllocator/MSLargeObjectSpace.h"
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

// Helper: check if an address belongs to any managed memory pool (main heap or LOS).
// Used in GC tracing paths to replace Heap::IsHeapAddress() which misses LOS objects.
static inline bool IsManagedAddress(MAddress addr)
{
    if (Heap::IsHeapAddress(addr)) {
        return true;
    }
    // Fallback: check LOS (Tier 2 huge objects allocated via mmap).
    // LOS::Contains acquires a mutex+linear scan, but this path is only
    // reached for addresses outside the contiguous heap — rare during tracing.
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    LargeObjectSpace* los = space.GetLargeObjectSpace();
    return los != nullptr && los->Contains(addr);
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
//   Stop all sweeps, clear markBitmaps, reclassify unswept→swept.
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

    // Stop sweep → clear bitmaps → reclassify → re-enable lazy sweep.
    // Safe pre-STW: no SweepRegion running, mark-at-alloc inactive.
    if (centralPool != nullptr) {
        centralPool->StopAndWaitForAllSweep();
        centralPool->ClearAllMarkBitmaps();
        centralPool->ReclassifyUnsweptToSwept();
        centralPool->ReenableLazySweep();
    }

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
        EnumAndMarkRoots(markWorkStack_);
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
// fall back to CopyGC RegionInfo for LOS (large object) regions.
// ============================================================================
bool MarkSweepCollector::MarkObject(BaseObject* obj) const
{
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);

    if (MSRegionInfoArray::IsHeapAddress(addr)) {
        return MSRegionInfoArray::MarkObjectInHeap(addr);
    }

    // Outside heap range: LOS Tier 2 (>4MB mmap objects) or external.
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    LargeObjectSpace* los = space.GetLargeObjectSpace();
    if (los != nullptr && los->MarkObject(addr)) {
        return false;  // newly marked in LOS
    }

    return true;
}

bool MarkSweepCollector::IsScannedObject(const BaseObject* obj) const
{
    if (obj == nullptr) {
        return true;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (MSRegionInfoArray::IsHeapAddress(addr)) {
        return MSRegionInfoArray::IsScannedObjectInHeap(addr);
    }

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    LargeObjectSpace* los = space.GetLargeObjectSpace();
    if (los != nullptr && los->Contains(addr)) {
        return true;
    }
    return true;
}

void MarkSweepCollector::SetScannedObject(const BaseObject* obj) const
{
    if (obj == nullptr) {
        return;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (MSRegionInfoArray::IsHeapAddress(addr)) {
        MSRegionInfoArray::SetScannedObjectInHeap(addr);
    }
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
        centralPool->VisitAllocBufferMS([&workStack](AllocBufferMS& buffer) { buffer.MergeRoots(workStack); });
    }
}

// ============================================================================
// Step 2.2: ConcurrentMark
// ============================================================================

void MarkSweepCollector::ConcurrentMark()
{
    // activeCollector_ was already set in MarkSetup (before STW).

    // 8. Transition to concurrent marking phase
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_CONCURRENT_MARK, true);

    // 9. Enable Mutator assist
    assistMarkedCount_.store(0, std::memory_order_relaxed);
    assistCallTotal_.store(0, std::memory_order_relaxed);
    assistProcessedCount_.store(0, std::memory_order_relaxed);
    assistTracedCount_.store(0, std::memory_order_relaxed);
    assistFastSkipCount_.store(0, std::memory_order_relaxed);
    globalShareCount_.store(0, std::memory_order_relaxed);
    globalSharedBufCount_.store(0, std::memory_order_relaxed);
    globalPopHitCount_.store(0, std::memory_order_relaxed);
    globalPopMissCount_.store(0, std::memory_order_relaxed);
    globalWorkAvailable_.store(false, std::memory_order_relaxed);
    assistEnabled_.store(true, std::memory_order_release);

    // 11. Run concurrent marking loop (GC thread + Mutator assist)
    RunConcurrentMarkLoop(markWorkStack_);

    // 12. Disable Mutator assist and clear activeCollector_.
    //     No longer needed beyond concurrent mark.
    assistEnabled_.store(false, std::memory_order_release);
}

void MarkSweepCollector::RunConcurrentMarkLoop(WorkStack& workStack)
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- ConcurrentMark (GC thread + Mutator assist) ---");
    VLOG(REPORT, "[MS-GC] ConcurrentMark initial workStack bufs: %zu", workStack.size());

    convergenceStableRounds_ = 0;

    while (true) {
        // 1. Share work: push half of GC local to global for Mutator assist
        if (workStack.size() >= kShareWorkBufs) {
            ShareWorkToGlobal(workStack);
        }

        // 2. Drain a slice from GC local work (unchanged logic)
        if (!workStack.empty()) {
            RunCollectorDrainSlice(workStack, kSliceMaxObjects);
        }

        // 3. If GC local empty, try refill from global queue
        if (workStack.empty()) {
            PopFromGlobal(workStack, 1);  // refill 1 buffer (~64 objects)
        }

        // 4. Convergence check: local AND global both empty, stable for N rounds
        bool globalEmpty = false;
        {
            std::lock_guard<std::mutex> lock(globalWorkMutex_);
            globalEmpty = globalWorkQueue_.empty();
        }

        if (workStack.empty() && globalEmpty) {
            convergenceStableRounds_++;
            if (convergenceStableRounds_ >= kConvergenceRequiredRounds) {
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

    cycleStats_.concurrentMarkUs = TimeUtil::MicroSeconds() - t0;
    VLOG(REPORT,
         "[MS-GC] ConcurrentMark time: %sus, marked %zu objects "
         "(assist: %zu calls, %zu marked, %zu processed, %zu traced, %zu fast-skip; "
         "global: %zu push/%zu bufs, pop hit %zu miss %zu)",
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         markedObjectCount.load(std::memory_order_relaxed),
         cycleStats_.assistCallCount,
         cycleStats_.assistMarkedObjects,
         cycleStats_.assistProcessedObjects,
         cycleStats_.assistTracedObjects,
         cycleStats_.assistFastSkipCount,
         cycleStats_.globalShareCount,
         cycleStats_.globalSharedBufs,
         cycleStats_.globalPopHitCount,
         cycleStats_.globalPopMissCount);
}

void MarkSweepCollector::RunCollectorDrainSlice(WorkStack& workStack, size_t maxObjects)
{
    size_t nNewlyMarked = 0;
    size_t processed = 0;
    while (!workStack.empty() && processed < maxObjects) {
        BaseObject* obj = workStack.back();
        workStack.pop_back();

        if (obj == nullptr || !IsManagedAddress(reinterpret_cast<MAddress>(obj))) {
            continue;
        }

        // Guard: objects in the mark queue must have a valid stateWord.
        // A zeroed stateWord means a corrupted/prematurely-reclaimed object —
        // this is a fatal GC invariant violation, not a recoverable condition.
        if (UNLIKELY(!obj->IsValidObject())) {
            LOG(RTLOG_FATAL, "[MS-GC] RunCollectorDrainSlice: FATAL invalid stateWord obj=%p, "
                "stateWord=0x%zx, markBit=%d",
                (void*)obj,
                *reinterpret_cast<uintptr_t*>(obj),
                (int)IsMarkedObject(obj));
        }

        // Step8 foundation: mark bit means "discovered" (white->grey),
        // scanned bit means children already traced (grey->black).
        bool alreadyMarked = MarkObject(obj);
        if (!alreadyMarked) {
            nNewlyMarked++;
        }
        if (alreadyMarked && IsScannedObject(obj)) {
            continue;
        }
        processed++;

        if (!obj->HasRefField()) {
            SetScannedObject(obj);
            continue;
        }

        // Handle WeakRef specially: don't trace referent, defer to WeakRefBuffer
        if (UNLIKELY(obj->IsWeakRef())) {
            RefField<>* referentField =
                reinterpret_cast<RefField<>*>(reinterpret_cast<uintptr_t>(obj) + sizeof(uintptr_t));
            BaseObject* referent = GetAndTryTagObj(obj, *referentField);
            if (referent != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(referent))) {
                // Don't mark referent — add to WeakRefBuffer for later processing
                WeakRefBuffer::Instance().Insert(obj);
            }
            SetScannedObject(obj);
            continue;
        }

        // Trace all reference fields: push children as grey (mark-on-pop)
        TraceObjectRefFields(obj, workStack);
        SetScannedObject(obj);
    }
    // Batch update: one atomic op per slice instead of per-object (same as CopyGC)
    (void)markedObjectCount.fetch_add(nNewlyMarked, std::memory_order_relaxed);
}

void MarkSweepCollector::TraceRefField(
    BaseObject* obj, RefField<>& field, WorkStack& workStack)
{
    BaseObject* target = field.GetTargetObject();
    if (target == nullptr || !IsManagedAddress(reinterpret_cast<MAddress>(target))) {
        return;
    }
    // Fatal: a traced reference field points to an object with invalid stateWord.
    // This indicates stale data in a live object — a GC invariant violation.
    if (UNLIKELY(!target->IsValidObject())) {
        LOG(RTLOG_FATAL, "[MS-GC] TraceRefField: FATAL INVALID target=%p "
            "(stateWord=0x%zx) from parent=%p (parentSW=0x%zx, parentTI=%p), "
            "fieldAddr=%p",
            (void*)target,
            *reinterpret_cast<uintptr_t*>(target),
            (void*)obj,
            *reinterpret_cast<uintptr_t*>(obj),
            (obj != nullptr ? (void*)obj->GetTypeInfo() : nullptr),
            (void*)&field);
    }
    // Push if not yet marked; actual marking happens on pop (mark-on-pop pattern,
    // same as CopyGC — avoids per-child atomic mark + fetch_add in hot loop)
    bool targetMarked = IsMarkedObject(target);
    if (UNLIKELY(IsHashMapTraceEnabled()) &&
        HashMapTracePermit() &&
        (IsInterestingDiagType(obj) || IsInterestingDiagType(target))) {
        VLOG(REPORT, "[MS-HM-DIAG] TraceRefField parent=%p<%s> target=%p<%s> marked=%d field=%p",
             (void*)obj, GetDiagTypeName(obj),
             (void*)target, GetDiagTypeName(target),
             (int)targetMarked, (void*)&field);
    }
    if (!targetMarked) {
        workStack.push_back(target);
    }
}

// ============================================================================
// Global work queue helpers (GC thread + Mutator assist)
// ============================================================================

void MarkSweepCollector::PushToGlobal(WorkStack& source)
{
    if (source.empty()) {
        return;
    }
    size_t movedBufs = source.size();
    std::lock_guard<std::mutex> lock(globalWorkMutex_);
    globalWorkQueue_.insert(source);
    globalWorkAvailable_.store(true, std::memory_order_release);
    // insert() transfers ownership and cleans source
    globalShareCount_.fetch_add(1, std::memory_order_relaxed);
    globalSharedBufCount_.fetch_add(movedBufs, std::memory_order_relaxed);
}

void MarkSweepCollector::PushSingleToGlobal(BaseObject* obj)
{
    std::lock_guard<std::mutex> lock(globalWorkMutex_);
    globalWorkQueue_.push_back(obj);
    globalWorkAvailable_.store(true, std::memory_order_release);
}

void MarkSweepCollector::EnqueueGreyObject(BaseObject* obj)
{
    PushSingleToGlobal(obj);
}

void MarkSweepCollector::FlushWriteBarrierOverflow(WriteBarrierBuffer& wbBuf)
{
    std::lock_guard<std::mutex> lock(globalWorkMutex_);
    wbBuf.Flush(globalWorkQueue_);
    if (!globalWorkQueue_.empty()) {
        globalWorkAvailable_.store(true, std::memory_order_release);
    }
}

void MarkSweepCollector::PopFromGlobal(WorkStack& dest, size_t maxBufs)
{
    std::lock_guard<std::mutex> lock(globalWorkMutex_);
    if (globalWorkQueue_.empty()) {
        globalWorkAvailable_.store(false, std::memory_order_release);
        globalPopMissCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    size_t toSplit = std::min(maxBufs, globalWorkQueue_.size());
    auto* bufs = globalWorkQueue_.split(toSplit);
    if (bufs != nullptr) {
        WorkStack tmp(bufs);
        dest.insert(tmp);
        globalPopHitCount_.fetch_add(1, std::memory_order_relaxed);
    }
    globalWorkAvailable_.store(!globalWorkQueue_.empty(), std::memory_order_release);
}

void MarkSweepCollector::ShareWorkToGlobal(WorkStack& local)
{
    size_t toShare = local.size() / 2;
    if (toShare == 0) {
        return;
    }
    auto* bufs = local.split(toShare);
    if (bufs == nullptr) {
        return;
    }
    WorkStack shared(bufs);
    std::lock_guard<std::mutex> lock(globalWorkMutex_);
    globalWorkQueue_.insert(shared);
    globalWorkAvailable_.store(true, std::memory_order_release);
    globalShareCount_.fetch_add(1, std::memory_order_relaxed);
    globalSharedBufCount_.fetch_add(toShare, std::memory_order_relaxed);
}

// ============================================================================
// Mutator assist: called from AllocBufferMS::Allocate() during concurrent mark
// Budget-limited: processes at most kAssistMaxObjectsPerCall grey objects.
// ============================================================================

void MarkSweepCollector::MSMarkAssist()
{
    if (!assistEnabled_.load(std::memory_order_acquire)) {
        return;
    }

    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) {
        return;
    }

    size_t callSeq = assistCallTotal_.fetch_add(1, std::memory_order_relaxed) + 1;

    WorkStack& localWork = mutator->GetAssistLocalWork();

    // Refill from global only when this mutator has no persisted assist work.
    // If STW interrupted a previous assist in-flight, the remaining local work
    // stays in Mutator::assistLocalWork_ and is resumed here or flushed at
    // MarkTermination under STW.
    if (localWork.empty()) {
        if (!globalWorkAvailable_.load(std::memory_order_acquire) &&
            (callSeq % kAssistProbeIntervalCalls) != 0) {
            assistFastSkipCount_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        PopFromGlobal(localWork, 1);
    }
    if (localWork.empty()) {
        return;  // No grey work available
    }

    size_t nNewlyMarked = 0;
    size_t nProcessed = 0;
    size_t nTraced = 0;
    while (nProcessed < kAssistMaxObjectsPerCall && !localWork.empty()) {
        BaseObject* obj = localWork.back();
        localWork.pop_back();

        if (obj == nullptr || !IsManagedAddress(reinterpret_cast<MAddress>(obj))) {
            continue;
        }

        // Guard: objects in the mark queue must have a valid stateWord (same as RunCollectorDrainSlice).
        if (UNLIKELY(!obj->IsValidObject())) {
            LOG(RTLOG_FATAL, "[MS-GC] MSMarkAssist: FATAL invalid stateWord obj=%p, "
                "stateWord=0x%zx, markBit=%d",
                (void*)obj,
                *reinterpret_cast<uintptr_t*>(obj),
                (int)IsMarkedObject(obj));
        }

        bool alreadyMarked = MarkObject(obj);
        if (!alreadyMarked) {
            nNewlyMarked++;
        }
        if (alreadyMarked && IsScannedObject(obj)) {
            continue;
        }
        nProcessed++;

        if (!obj->HasRefField()) {
            SetScannedObject(obj);
            continue;
        }

        // WeakRef: defer to GC main thread (WeakRefBuffer may not be thread-safe)
        if (UNLIKELY(obj->IsWeakRef())) {
            PushSingleToGlobal(obj);
            continue;
        }

        // Trace children → push grey objects to localWork (mark-on-pop)
        TraceObjectRefFields(obj, localWork);
        SetScannedObject(obj);
        nTraced++;

        // Flush excess to global to share with GC thread and other assists
        if (localWork.size() > kAssistLocalFlushBufs) {
            ShareWorkToGlobal(localWork);
        }
    }

    // Push remaining local work back to global
    if (!localWork.empty()) {
        PushToGlobal(localWork);
    }

    // Batch update: one atomic op per assist call instead of per-object
    (void)markedObjectCount.fetch_add(nNewlyMarked, std::memory_order_relaxed);
    assistMarkedCount_.fetch_add(nNewlyMarked, std::memory_order_relaxed);
    assistProcessedCount_.fetch_add(nProcessed, std::memory_order_relaxed);
    assistTracedCount_.fetch_add(nTraced, std::memory_order_relaxed);
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

    WorkStack mainQueue = NewWorkStack();
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
    //    persisted assist-local work + globalWorkQueue_.
    {
        FlushAllAssistLocalWorks(mainQueue);
        DrainGlobalWorkQueueToMainQueue(mainQueue);
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
        // Finalize the next sweep generation before mutators resume.
        centralPool->FlushAllAllocBuffers();
        centralPool->PrepareForSweep();
    }

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

void MarkSweepCollector::CollectUnmarkedToMainQueue(WorkStack& mainQueue)
{
    // Collect extern (foreign) object candidates
    CollectExternCandidatesToMain(mainQueue);

    // Collect finalizer resurrection candidates
    CollectResurrectionCandidatesToMain(mainQueue);
}

void MarkSweepCollector::FlushAllWriteBarrierBuffers(WorkStack& mainQueue)
{
    auto func = [&mainQueue](Mutator& mutator) {
        mutator.GetWriteBarrierBuffer().Flush(mainQueue);
    };
    MutatorManager::Instance().VisitAllMutators(func);
}
void MarkSweepCollector::FlushAllAssistLocalWorks(WorkStack& mainQueue)
{
    auto func = [&mainQueue](Mutator& mutator) {
        mainQueue.insert(mutator.GetAssistLocalWork());
    };
    MutatorManager::Instance().VisitAllMutators(func);
}

void MarkSweepCollector::DrainGlobalWorkQueueToMainQueue(WorkStack& mainQueue)
{
    std::lock_guard<std::mutex> lock(globalWorkMutex_);
    while (!globalWorkQueue_.empty()) {
        BaseObject* obj = globalWorkQueue_.back();
        globalWorkQueue_.pop_back();
        if (obj != nullptr) {
            mainQueue.push_back(obj);
        }
    }
}

void MarkSweepCollector::CollectExternCandidatesToMain(WorkStack& mainQueue)
{
    for (auto& kv : discoveredExternObjects) {
        for (BaseObject* obj : kv.second) {
            if (obj != nullptr && obj->IsValidObject() && !IsMarkedObject(obj)) {
                mainQueue.push_back(obj);
            }
        }
    }
}

void MarkSweepCollector::CollectResurrectionCandidatesToMain(WorkStack& mainQueue)
{
    VisitFinalizerRoots([&mainQueue, this](ObjectRef& ref) {
        BaseObject* finalizerObj = ref.object;
        if (finalizerObj != nullptr && finalizerObj->IsValidObject() && !IsMarkedObject(finalizerObj)) {
            mainQueue.push_back(finalizerObj);
        }
    });
}

void MarkSweepCollector::ExecuteLikeDrainMainQueue(WorkStack& mainQueue)
{
    size_t processed = 0;
    size_t nNewlyMarked = 0;

    while (!mainQueue.empty()) {
        BaseObject* obj = mainQueue.back();
        mainQueue.pop_back();
        processed++;
        if (obj == nullptr || !IsManagedAddress(reinterpret_cast<MAddress>(obj))) {
            continue;
        }

        // Guard: objects in released/reclaimed regions must not appear in
        // the mark queue. A zeroed stateWord is a fatal GC invariant violation.
        if (!obj->IsValidObject()) {
            LOG(RTLOG_FATAL, "[MS-GC] ExecuteLikeDrainMainQueue: FATAL invalid stateWord obj=%p, "
                "stateWord=0x%zx",
                (void*)obj,
                *reinterpret_cast<uintptr_t*>(obj));
        }

        // Step8 foundation: discovered (mark) is separate from scanned.
        // Only objects already scanned can be skipped safely.
        bool alreadyMarked = MarkObject(obj);
        if (!alreadyMarked) {
            nNewlyMarked++;
        }
        if (alreadyMarked && IsScannedObject(obj)) {
            continue;
        }

        if (UNLIKELY(IsHashMapTraceEnabled()) && HashMapTracePermit() && IsInterestingDiagType(obj)) {
            VLOG(REPORT, "[MS-HM-DIAG] DrainObj obj=%p<%s> alreadyMarked=%d qRemain=%zu",
                 (void*)obj, GetDiagTypeName(obj), (int)alreadyMarked, mainQueue.size());
        }

        if (!obj->HasRefField()) {
            SetScannedObject(obj);
            continue;
        }

        if (UNLIKELY(obj->IsWeakRef())) {
            RefField<>* referentField =
                reinterpret_cast<RefField<>*>(reinterpret_cast<uintptr_t>(obj) + sizeof(uintptr_t));
            BaseObject* referent = GetAndTryTagObj(obj, *referentField);
            if (referent != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(referent))) {
                TraceObjectRefFields(referent, mainQueue);
                WeakRefBuffer::Instance().Insert(obj);
            }
            SetScannedObject(obj);
            continue;
        }

        TraceObjectRefFields(obj, mainQueue);
        SetScannedObject(obj);
    }

    cycleStats_.markTermProcessedObjects = processed;
    // Batch update: one atomic op for entire drain
    (void)markedObjectCount.fetch_add(nNewlyMarked, std::memory_order_relaxed);
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

    // 6. Large object garbage collection (eager sweep for LOS) — with stats
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
    GCPhase phase = Mutator::GetMutator()->GetMutatorPhase();
    if (UNLIKELY(phase == GCPhase::GC_PHASE_MS_MARK_SETUP) ||
        UNLIKELY(phase == GCPhase::GC_PHASE_MS_CONCURRENT_MARK) ||
        UNLIKELY(phase == GCPhase::GC_PHASE_MS_MARK_TERMINATION)) {
        MarkObject(obj);
        SetScannedObject(obj);
    }
}

// ============================================================================
// TracingCollector virtual overrides — simplified for Mark-Sweep (no tag/CAS)
// ============================================================================

void MarkSweepCollector::EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const
{
    BaseObject* target = ref.GetTargetObject();
    if (target != nullptr) {
        rootSet.push_back(target);
    }
}

void MarkSweepCollector::EnumAndTagRawRoot(ObjectRef& root, RootSet& rootSet) const
{
    // MS version: no tag/CAS, just push heap objects into root set
    BaseObject* obj = root.object;
    if (obj != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(obj))) {
        rootSet.push_back(obj);
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
