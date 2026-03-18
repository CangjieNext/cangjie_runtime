// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MarkSweepCollector.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "CangjieRuntime.h"
#include "Heap/Allocator/CentralRegionPool.h"
#include "Heap/Allocator/LargeObjectSpace.h"
#include "Heap/Allocator/MSRegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Barrier/WriteBarrierBuffer.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Collector/ForwardDataManager.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"

#ifndef _WIN32
#include <unistd.h>  // usleep
#endif

namespace MapleRuntime {

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
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
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
}

void MarkSweepCollector::Fini()
{
    LOG(RTLOG_REPORT, "[MS-GC] MarkSweepCollector::Fini()");
    TracingCollector::Fini();
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

    g_gcCount++;
    g_gcTotalTimeUs += gcTimeUs;
    g_gcCollectedTotalBytes += gcStats.collectedBytes;
}

void MarkSweepCollector::DoGarbageCollection(uint64_t gcIndex)
{
    (void)gcIndex;

    MarkSetup();           // Step 2.1
    ConcurrentMark();      // Step 2.2
    MarkTermination();     // Step 2.3
    PostMarkTermination(); // Step 2.4
    ConcurrentSweep();      // Step 4: concurrent sweep via gc-helper
}

// ============================================================================
// Step 2.1: MarkSetup
//   Phase A  (pre-handshake): ForceFinishSweep
//   Phase B  (brief STW):     Flush all TL Regions (only — minimal STW scope)
//   Phase B2 (post-STW):      Clear all mark bitmaps + LOS marks (no STW needed)
//   Phase C  (handshake):     GcPhaseEnum — each Mutator scans its own stack roots
//   Phase D  (post-handshake): EnumAndMarkRoots
// ============================================================================

void MarkSweepCollector::MarkSetup()
{
    uint64_t phaseStart = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- MarkSetup ---");

    // Set activeCollector_ early so that write barrier overflow handling
    // (MarkAndEnqueue → FlushWriteBarrierOverflow) can reach the global work
    // queue as soon as mutators resume with MARK_SETUP phase after STW.
    activeCollector_.store(this, std::memory_order_release);

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();

    // ── Phase A: Ensure previous sweep is fully completed ──
    //    Must happen BEFORE bitmap clear (sweep reads the old bitmap).
    //    Corresponds to Go's finishsweep_m().
    {
        uint64_t t0 = TimeUtil::MicroSeconds();
        if (centralPool != nullptr) {
            centralPool->WaitForSweepDone();
            centralPool->ForceFinishSweep();
        }
        cycleStats_.forceFinishSweepUs = TimeUtil::MicroSeconds() - t0;
        VLOG(REPORT, "[MS-GC] ForceFinishSweep: %sus",
             Pretty(cycleStats_.forceFinishSweepUs).Str());
    }

    // ── Phase B: Brief STW — Flush TL Regions + Clear mark bitmaps ──
    //    Why STW:  AllocBufferMS is per-OS-thread (TLS), shared by N coroutines
    //    on the same Processor. Handshake is per-Mutator (per-coroutine), so
    //    we cannot safely flush from HandleGCPhase (wrong TLS for parked coroutines,
    //    and Mutators resume immediately after handshake — concurrent allocation).
    //    A brief STW ensures no Mutator is allocating during flush.
    //    ClearMarkBitmaps MUST be inside STW: after FlushAllAllocBuffers returns
    //    ALL regions to central pool, VisitAllRegions can then clear every region.
    //    If done outside STW, mutators may take regions before the clear, leaving
    //    stale mark bits that prevent root re-tracing in subsequent cycles.
    {
        ScopedStopTheWorld stw("MS FlushTLRegions + ClearBitmaps");

        // Flush all per-Processor AllocBufferMS Regions back to CentralRegionPool.
        // After this, ALL small-object Regions are in the central pool.
        if (centralPool != nullptr) {
            centralPool->FlushAllAllocBuffers();
        }

        // Clear all MSRegionInfo markBitmaps (small objects)
        // Must be done while ALL regions are in central pool (i.e., during STW).
        {
            uint64_t t0 = TimeUtil::MicroSeconds();
            size_t clearedRegions = 0;
            if (centralPool != nullptr) {
                centralPool->VisitAllRegions([&clearedRegions](MSRegionInfo* region) {
                    size_t wordCount = (region->totalSlotCount + 63) / 64;
                    if (region->markBitmap != nullptr && wordCount > 0) {
                        memset(region->markBitmap, 0, wordCount * sizeof(uint64_t));
                    }
                    clearedRegions++;
                });
            }
            cycleStats_.clearBitmapUs = TimeUtil::MicroSeconds() - t0;
            cycleStats_.clearBitmapRegions = clearedRegions;
            VLOG(REPORT, "[MS-GC] ClearMarkBitmaps: %zu regions, %sus",
                 clearedRegions, Pretty(cycleStats_.clearBitmapUs).Str());
        }

        // Clear LOS marks (Tier 2: large objects use region-level isMarked flag)
        LargeObjectSpace* los = space.GetLargeObjectSpace();
        if (los != nullptr) {
            los->ClearAllMarks();
        }

        // Clear Tier 1 in-heap large region marks
        if (centralPool != nullptr) {
            centralPool->ClearLargeMarks();
        }

        // Reset marked object counter (before root scanning below)
        markedObjectCount.store(0, std::memory_order_relaxed);

        // ── Phase C: Transition to MS_MARK_SETUP while world is stopped ──
        //    This MUST happen inside STW so that when mutators resume, they go
        //    through HandleSafepointStub.S → MRT_GetThreadLocalData(), which
        //    encodes GCPhase (20) into x28's high 8 bits. The compiler's write
        //    barrier fast path checks x28>>56 > 8 to decide whether to call the
        //    barrier slow path. If the transition happens OUTSIDE STW (as a
        //    separate handshake), mutators can resume from STW with x28 still
        //    encoding the old phase (≤8), causing the fast path to skip the
        //    barrier entirely during concurrent marking.
        //
        //    HandleGCPhase(MS_MARK_SETUP) → GcPhaseEnum: the GC thread scans
        //    each stopped mutator's stack roots on their behalf. This is safe
        //    because all mutators are frozen during STW.
        TransitionToGCPhase(GCPhase::GC_PHASE_MS_MARK_SETUP, true);
    } // ~ScopedStopTheWorld → StartTheWorld
      // Mutators resume through HandleSafepointStub.S → MRT_GetThreadLocalData()
      // → x28 high 8 bits = 20 (MS_MARK_SETUP) → barrier fast path fires

    // ── Phase D: GC thread enumerates global roots + marks root set ──
    markWorkStack_.clear();
    markForeignStack_.clear();
    {
        uint64_t t0 = TimeUtil::MicroSeconds();
        EnumAndMarkRoots(markWorkStack_, markForeignStack_);
        cycleStats_.enumRootsUs = TimeUtil::MicroSeconds() - t0;
        cycleStats_.rootsCount = markWorkStack_.size();
    }

    cycleStats_.markSetupUs = TimeUtil::MicroSeconds() - phaseStart;

    VLOG(REPORT, "[MS-GC] MarkSetup total: %sus, roots=%zu",
         Pretty(cycleStats_.markSetupUs).Str(), cycleStats_.rootsCount);
}

void MarkSweepCollector::ClearAllMarkBitmaps()
{
    // Mark bitmaps are now cleared in MarkSetup via CentralRegionPool::VisitAllRegions.
    // This function is a no-op stub for interface compatibility.
}

void MarkSweepCollector::EnsureSweepDone()
{
    // Sweep completion is now ensured in MarkSetup via CentralRegionPool::ForceFinishSweep().
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
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    LargeObjectSpace* los = space.GetLargeObjectSpace();
    if (los != nullptr && los->MarkObject(addr)) {
        return false;  // newly marked in LOS
    }

    return true;
}


void MarkSweepCollector::EnumAndMarkRoots(WorkStack& workStack, WorkStack& foreignStack)
{
    MRT_PHASE_TIMER("MS EnumAndMarkRoots");

    // Step A: Root enumeration — reuse TracingCollector::DoEnumeration
    // This will: enumerate static roots, coroutine roots, finalizer roots,
    // merge mutator stack roots (from GcPhaseEnum done during handshake)
    DoEnumeration(workStack, foreignStack);

    // Step B: Mark root objects — set mark bit for each root
    // Drain workStack into temp vector, then re-push only newly-marked roots
    // (MarkStack has no move assignment operator, so we drain-and-refill)
    std::vector<BaseObject*> roots;
    while (!workStack.empty()) {
        roots.push_back(workStack.back());
        workStack.pop_back();
    }

    for (BaseObject* obj : roots) {
        if (obj != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(obj))) {
            // Always mark and always push — ensure all roots are traced
            // even if MarkObject reports them as already marked (ROOT CAUSE #4 investigation)
            bool alreadyMarked = MarkObject(obj);
            if (!alreadyMarked) {
                markedObjectCount.fetch_add(1, std::memory_order_relaxed);
            }
            workStack.push_back(obj);
        }
    }

    // Mark foreign (export) roots too
    std::vector<BaseObject*> fRoots;
    while (!foreignStack.empty()) {
        fRoots.push_back(foreignStack.back());
        foreignStack.pop_back();
    }
    for (BaseObject* obj : fRoots) {
        if (obj != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(obj))) {
            if (!MarkObject(obj)) {
                markedObjectCount.fetch_add(1, std::memory_order_relaxed);
                foreignStack.push_back(obj);
            }
        }
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

    // 9. Merge foreignStack into workStack
    while (!markForeignStack_.empty()) {
        BaseObject* obj = markForeignStack_.back();
        markForeignStack_.pop_back();
        markWorkStack_.push_back(obj);
    }

    // 10. Enable Mutator assist
    assistMarkedCount_.store(0, std::memory_order_relaxed);
    assistCallTotal_.store(0, std::memory_order_relaxed);
    assistEnabled_.store(true, std::memory_order_release);

    // 11. Run concurrent marking loop (GC thread + Mutator assist)
    RunConcurrentMarkLoop(markWorkStack_);

    // 12. Disable Mutator assist (but keep activeCollector_ alive —
    //     it will be cleared in MarkTermination under STW to avoid
    //     a window where mutators can't flush WB overflow to global queue).
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
        if (workStack.size() > kShareWorkBufs) {
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

    cycleStats_.concurrentMarkUs = TimeUtil::MicroSeconds() - t0;
    VLOG(REPORT, "[MS-GC] ConcurrentMark time: %sus, marked %zu objects (assist: %zu calls, %zu marked)",
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         markedObjectCount.load(std::memory_order_relaxed),
         cycleStats_.assistCallCount,
         cycleStats_.assistMarkedObjects);
}

void MarkSweepCollector::RunCollectorDrainSlice(WorkStack& workStack, size_t maxObjects)
{
    size_t processed = 0;
    while (!workStack.empty() && processed < maxObjects) {
        BaseObject* obj = workStack.back();
        workStack.pop_back();

        if (obj == nullptr || !IsManagedAddress(reinterpret_cast<MAddress>(obj))) {
            continue;
        }

        // Object is grey (marked but not yet traced). Trace its children.
        processed++;

        // DEBUG: validate TypeInfo before touching it
        {
            TypeInfo* ti = obj->GetTypeInfo();
            uintptr_t tiAddr = reinterpret_cast<uintptr_t>(ti);
            if (ti == nullptr || tiAddr < 0x100000000ULL) {
                uintptr_t objAddr = reinterpret_cast<uintptr_t>(obj);
                // Read raw stateWord to understand corruption
                uint64_t rawSW = *reinterpret_cast<uint64_t*>(obj);
                MSRegionInfo* msInfo = MSRegionInfoArray::GetRegionInfoValidated(objAddr);
                LOG(RTLOG_ERROR,
                    "DrainSlice: obj=%p has invalid TypeInfo=%p (raw stateword=0x%llx), "
                    "msRegion: type=%d, sizeClass=%u, allocPtr=0x%zx, regionEnd=0x%zx, "
                    "sweepState=%u, sweepGen=%u, markBitmap=%p, totalSlots=%u, freeSlots=%u",
                    (void*)obj, (void*)ti,
                    (unsigned long long)rawSW,
                    msInfo ? (int)msInfo->regionType : -1,
                    msInfo ? (unsigned)msInfo->sizeClass : 0,
                    msInfo ? msInfo->allocPtr : 0,
                    msInfo ? msInfo->regionEnd : 0,
                    msInfo ? (unsigned)msInfo->sweepState : 0,
                    msInfo ? (unsigned)msInfo->sweepGen : 0,
                    msInfo ? (void*)msInfo->markBitmap : nullptr,
                    msInfo ? (unsigned)msInfo->totalSlotCount : 0,
                    msInfo ? (unsigned)msInfo->freeSlotCount : 0);
                continue;
            }
        }

        if (!obj->HasRefField()) {
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
            continue;
        }

        // Trace all reference fields: mark children and push newly-marked as grey
        TraceObjectRefFields(obj, workStack);
    }
}

void MarkSweepCollector::TraceRefField(
    BaseObject* obj, RefField<>& field, WorkStack& workStack)
{
    (void)obj; // obj is the parent, not needed for MS (no tag/CAS)
    BaseObject* target = field.GetTargetObject();
    if (target == nullptr || !IsManagedAddress(reinterpret_cast<MAddress>(target))) {
        return;
    }
    // Mark child: if newly marked (returns false), push as grey
    if (!MarkObject(target)) {
        markedObjectCount.fetch_add(1, std::memory_order_relaxed);
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
    std::lock_guard<std::mutex> lock(globalWorkMutex_);
    globalWorkQueue_.insert(source);
    // insert() transfers ownership and cleans source
}

void MarkSweepCollector::PushSingleToGlobal(BaseObject* obj)
{
    std::lock_guard<std::mutex> lock(globalWorkMutex_);
    globalWorkQueue_.push_back(obj);
}

void MarkSweepCollector::EnqueueGreyObject(BaseObject* obj)
{
    PushSingleToGlobal(obj);
}

void MarkSweepCollector::FlushWriteBarrierOverflow(WriteBarrierBuffer& wbBuf)
{
    std::lock_guard<std::mutex> lock(globalWorkMutex_);
    BaseObject** data = wbBuf.Data();
    size_t n = wbBuf.Count();
    for (size_t i = 0; i < n; ++i) {
        if (data[i] != nullptr) {
            globalWorkQueue_.push_back(data[i]);
        }
    }
    wbBuf.Clear();
}

void MarkSweepCollector::PopFromGlobal(WorkStack& dest, size_t maxBufs)
{
    std::lock_guard<std::mutex> lock(globalWorkMutex_);
    if (globalWorkQueue_.empty()) {
        return;
    }
    size_t toSplit = std::min(maxBufs, globalWorkQueue_.size());
    auto* bufs = globalWorkQueue_.split(toSplit);
    if (bufs != nullptr) {
        WorkStack tmp(bufs);
        dest.insert(tmp);
    }
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

    assistCallTotal_.fetch_add(1, std::memory_order_relaxed);

    WorkStack localWork;

    // Refill from global (1 buffer ≈ up to 64 objects)
    PopFromGlobal(localWork, 1);
    if (localWork.empty()) {
        return;  // No grey work available
    }

    size_t processed = 0;
    while (processed < kAssistMaxObjectsPerCall && !localWork.empty()) {
        BaseObject* obj = localWork.back();
        localWork.pop_back();

        if (obj == nullptr || !IsManagedAddress(reinterpret_cast<MAddress>(obj))) {
            continue;
        }

        processed++;

        if (!obj->HasRefField()) {
            continue;
        }

        // WeakRef: defer to GC main thread (WeakRefBuffer may not be thread-safe)
        if (UNLIKELY(obj->IsWeakRef())) {
            PushSingleToGlobal(obj);
            continue;
        }

        // Trace children → push newly-marked grey objects to localWork
        TraceObjectRefFields(obj, localWork);

        // Flush excess to global to share with GC thread and other assists
        if (localWork.size() > kAssistLocalFlushBufs) {
            ShareWorkToGlobal(localWork);
        }
    }

    // Push remaining local work back to global
    if (!localWork.empty()) {
        PushToGlobal(localWork);
    }

    assistMarkedCount_.fetch_add(processed, std::memory_order_relaxed);
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
    // This is safe because no mutator code (including write barriers) can run during STW.
    // We kept it alive through the ConcurrentMark→MarkTermination gap to ensure
    // write barrier overflow could always reach the global work queue.
    activeCollector_.store(nullptr, std::memory_order_release);

    WorkStack mainQueue = NewWorkStack();

    // 1. Flush all write barrier buffers (per-mutator)
    {
        uint64_t t0 = TimeUtil::MicroSeconds();
        FlushAllWriteBarrierBuffers(mainQueue);
        cycleStats_.wbBufFlushUs = TimeUtil::MicroSeconds() - t0;
        cycleStats_.wbBufEntriesTotal = mainQueue.size();
    }

    // 1b. Drain global work queue — objects pushed via WB overflow flush
    //     (FlushWriteBarrierOverflow) go directly to globalWorkQueue_ and are
    //     NOT in per-mutator WB buffers. Must drain them here under STW.
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

    // 2. Collect other remaining unmarked objects to main queue
    {
        uint64_t t0 = TimeUtil::MicroSeconds();
        CollectExternCandidatesToMain(mainQueue);
        CollectResurrectionCandidatesToMain(mainQueue);
        cycleStats_.drainedGreyObjects = mainQueue.size();
        cycleStats_.drainMarkQueueUs = TimeUtil::MicroSeconds() - t0;
    }

    // 3. Drain main queue with Execute-like loop (same as concurrent mark logic)
    {
        size_t beforeMark = markedObjectCount.load(std::memory_order_relaxed);
        ExecuteLikeDrainMainQueue(mainQueue);
        cycleStats_.drainedGreyObjects = markedObjectCount.load(std::memory_order_relaxed) - beforeMark;
    }

    cycleStats_.markTerminationUs = TimeUtil::MicroSeconds() - phaseStart;

    VLOG(REPORT, "[MS-GC] DrainMarkQueue: drained %zu grey objects, %sus",
         cycleStats_.drainedGreyObjects, Pretty(cycleStats_.drainMarkQueueUs).Str());
    VLOG(REPORT, "[MS-GC] FlushWriteBarrierBuffers (final): %zu entries, %sus",
         cycleStats_.wbBufEntriesTotal, Pretty(cycleStats_.wbBufFlushUs).Str());
    VLOG(REPORT, "[MS-GC] MarkTermination total: %sus, total marked %zu",
         Pretty(cycleStats_.markTerminationUs).Str(),
         markedObjectCount.load(std::memory_order_relaxed));

    // Transition to MS_SWEEP INSIDE STW so that when mutators resume,
    // the phase is MS_SWEEP (idle barrier). This eliminates a gap where
    // mutators could trigger the SATB barrier after MarkTermination drain
    // but before PostMarkTermination switches the phase — objects shaded
    // in that gap would never be traced because PostMarkTermination's
    // ClearAllWriteBarrierBuffers discards them without tracing.
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_SWEEP, false);
    ClearAllWriteBarrierBuffers();

    // 3. Transition to MS_SWEEP (uninstalls mark barrier, installs idle barrier)
    //    Note: ScopedStopTheWorld destructor will call StartTheWorld()
}

void MarkSweepCollector::CollectUnmarkedToMainQueue(WorkStack& mainQueue)
{
    // Flush all Mutators' write barrier buffers (replaces SATB collection)
    FlushAllWriteBarrierBuffers(mainQueue);

    // Collect extern (foreign) object candidates
    CollectExternCandidatesToMain(mainQueue);

    // Collect finalizer resurrection candidates
    CollectResurrectionCandidatesToMain(mainQueue);
}

void MarkSweepCollector::FlushAllWriteBarrierBuffers(WorkStack& mainQueue)
{
    // Under STW: flush each Mutator's WriteBarrierBuffer into mainQueue.
    // Objects are already marked by the write barrier; this just ensures
    // they get traced (their children discovered).
    auto func = [&mainQueue](Mutator& mutator) {
        mutator.GetWriteBarrierBuffer().Flush(mainQueue);
    };
    MutatorManager::Instance().VisitAllMutators(func);
}

void MarkSweepCollector::ClearAllWriteBarrierBuffers()
{
    auto func = [](Mutator& mutator) {
        mutator.GetWriteBarrierBuffer().Clear();
    };
    MutatorManager::Instance().VisitAllMutators(func);
}

void MarkSweepCollector::CollectExternCandidatesToMain(WorkStack& mainQueue)
{
    for (auto& kv : discoveredExternObjects) {
        for (BaseObject* obj : kv.second) {
            if (!IsMarkedObject(obj)) {
                mainQueue.push_back(obj);
            }
        }
    }
}

void MarkSweepCollector::CollectResurrectionCandidatesToMain(WorkStack& mainQueue)
{
    VisitFinalizerRoots([&mainQueue, this](ObjectRef& ref) {
        BaseObject* finalizerObj = ref.object;
        if (finalizerObj != nullptr && !IsMarkedObject(finalizerObj)) {
            mainQueue.push_back(finalizerObj);
        }
    });
}

void MarkSweepCollector::ExecuteLikeDrainMainQueue(WorkStack& mainQueue)
{
    while (!mainQueue.empty()) {
        BaseObject* obj = mainQueue.back();
        mainQueue.pop_back();

        if (obj == nullptr || !IsManagedAddress(reinterpret_cast<MAddress>(obj))) {
            continue;
        }

        // Try to mark. If newly marked, count it.
        // But ALWAYS trace children — objects from the write barrier buffer
        // are already marked by MarkAndEnqueue; skipping them would leave
        // their children untraced and subject to incorrect sweeping.
        bool alreadyMarked = MarkObject(obj);
        if (!alreadyMarked) {
            markedObjectCount.fetch_add(1, std::memory_order_relaxed);
        }

        if (!obj->HasRefField()) {
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
            continue;
        }

        TraceObjectRefFields(obj, mainQueue);
    }
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

    // 4. OHOS cross-language cycle reference preparation
    PrepareCycleRef();

    // 5. OHOS cross-language cycle reference resolve notification
    PostResolveCycleTask();

    // 6. Large object garbage collection (eager sweep for LOS) — with stats
    {
        uint64_t t0 = TimeUtil::MicroSeconds();
        CollectLargeGarbage();
        cycleStats_.losCollectUs = TimeUtil::MicroSeconds() - t0;
        VLOG(REPORT, "[MS-GC] CollectLargeGarbage: freed %zu large objects (%s B), %sus",
             cycleStats_.losFreedCount, Pretty(cycleStats_.losFreedBytes).Str(),
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
    FinalizerProcessor& fp = collectorResources.GetFinalizerProcessor();
    fp.EnqueueFinalizables(finalizable, snapshotFinalizerNum);
    fp.Notify();
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
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);

    // Tier 2: LOS huge objects (>4MB mmap)
    LargeObjectSpace* los = space.GetLargeObjectSpace();
    if (los != nullptr) {
        los->SweepUnmarkedObjectsWithStats(cycleStats_.losFreedCount, cycleStats_.losFreedBytes);
    }

    // Tier 1: in-heap large regions — eager sweep unmarked ones
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        size_t largeFreedBytes = centralPool->SweepLargeRegions();
        cycleStats_.losFreedBytes += largeFreedBytes;
    }
}

void MarkSweepCollector::ConcurrentSweep()
{
    uint64_t phaseStart = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- ConcurrentSweep ---");

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);

    // Compute true live bytes from markBitmaps BEFORE gen flip (bitmaps are stable here).
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        cycleStats_.trueLiveBytes = centralPool->ComputeMarkedBytes();
    }

    // Mark-Sweep: generation flip via CentralRegionPool::PrepareForSweep().
    // This swaps swept↔unswept lists for all 36 size-class buckets and bumps
    // globalSweepGen_ by 2.  Actual per-region sweeping happens lazily on the
    // allocation path (SweepAndClassify) or eagerly via BackgroundSweep.
    if (centralPool != nullptr) {
        // STW: Flush all per-thread AllocBufferMS regions to central pool AND
        // perform the generation flip (PrepareForSweep) atomically.
        // Without the flush, mutator-cached regions survive the gen flip.
        // Without STW around both, mutators can take regions from the pool
        // between flush and PrepareForSweep, causing those regions to miss
        // the gen flip entirely.
        {
            ScopedStopTheWorld stwFlush("MS FlushAndPrepSweep");
            centralPool->FlushAllAllocBuffers();
            centralPool->PrepareForSweep();
        }
        VLOG(REPORT, "[MS-GC] FlushAllAllocBuffers + PrepareForSweep (gen flip) done");

        // Notify gc-helper thread to start background sweep.
        FinalizerProcessor& fp = Heap::GetHeap().GetFinalizerProcessor();
        fp.NotifyToSweep();

        // Notify gc-helper thread to release dirty pages to OS.
        fp.NotifyToReclaimGarbage();
    }

    uint64_t sweepUs = TimeUtil::MicroSeconds() - phaseStart;
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
    VLOG(REPORT, "[MS-GC] allocated %s B, threshold %s B, gcIndex=%llu",
         Pretty(Heap::GetHeap().GetAllocatedSize()).Str(),
         Pretty(GetGCStats().GetThreshold()).Str(),
         static_cast<unsigned long long>(gcIndex));
}

// Snapshot previous cycle's sweep stats before PrepareForSweep resets them.
void MarkSweepCollector::SnapshotPreviousCycleSweepStats()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        const MSSweepStats& ss = centralPool->GetSweepStats();
        cycleStats_.prevBgSweepRegions = ss.bgSweptRegions.load(std::memory_order_relaxed);
        cycleStats_.prevLazySweepRegions = ss.lazySweptRegions.load(std::memory_order_relaxed);
        cycleStats_.prevForceSweepRegions = ss.forceSweptRegions.load(std::memory_order_relaxed);
        cycleStats_.prevSweptSmallRegions = ss.TotalSweptRegions();
        cycleStats_.prevSweptSmallFreedBytes = ss.sweptFreedBytes.load(std::memory_order_relaxed);
        cycleStats_.prevReleasedRegions = ss.releasedRegions.load(std::memory_order_relaxed);
    }
    // LOS stats come from the current cycle's CollectLargeGarbage (already in cycleStats_)
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
    size_t forceRegions = cycleStats_.prevForceSweepRegions;

    VLOG(REPORT, "[MS-GC] --- Sweep Summary (since cycle=%llu) ---",
         static_cast<unsigned long long>(gcCycle_ - 1));
    VLOG(REPORT, "[MS-GC] swept small: %zu regions, freed %s B (bitmap CTZ sweep)",
         totalSwept, Pretty(cycleStats_.prevSweptSmallFreedBytes).Str());
    VLOG(REPORT, "[MS-GC] swept large: %zu objects, freed %s B",
         cycleStats_.prevSweptLargeCount, Pretty(cycleStats_.prevSweptLargeFreedBytes).Str());
    VLOG(REPORT, "[MS-GC] released regions: %zu (returned to OS)", cycleStats_.prevReleasedRegions);

    if (totalSwept > 0) {
        size_t bgPct = bgRegions * 100 / totalSwept;
        size_t lazyPct = lazyRegions * 100 / totalSwept;
        size_t forcePct = forceRegions * 100 / totalSwept;
        VLOG(REPORT, "[MS-GC] sweep breakdown: background %zu (%zu%%), lazy %zu (%zu%%), force %zu (%zu%%)",
             bgRegions, bgPct, lazyRegions, lazyPct, forceRegions, forcePct);
    } else {
        VLOG(REPORT, "[MS-GC] sweep breakdown: no regions swept");
    }
}

// Block ⑤: Region Statistics (CentralRegionPool 4-list view)
void MarkSweepCollector::DumpRegionStatsMS()
{
    uint32_t sweepGen = CentralRegionPool::GetGlobalSweepGen();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);

    VLOG(REPORT, "[MS-GC] region statistics when gc ends (sweepGen=%u)", sweepGen);

    // RegionManager basic stats (total/active units)
    size_t totalSize = space.GetMaxCapacity();
    size_t heapSize = space.GetUsedPageSize();
    size_t allocatedSize = space.AllocatedBytes();
    VLOG(REPORT, "[MS-GC]   total capacity: %s B", Pretty(totalSize).Str());
    VLOG(REPORT, "[MS-GC]   used pages: %s B, allocated: %s B", Pretty(heapSize).Str(), Pretty(allocatedSize).Str());

    // CentralRegionPool 4-list breakdown
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->DumpPoolStats();
    }

    // LOS stats
    LargeObjectSpace* los = space.GetLargeObjectSpace();
    if (los != nullptr) {
        VLOG(REPORT, "[MS-GC]   large-object regions: %zu objects, %s B mapped",
             los->GetObjectCount(), Pretty(los->GetTotalMappedSize()).Str());
    }
}

// Block ⑥: GC Summary — 3 lines
void MarkSweepCollector::LogGCSummary(uint64_t gcTimeUs)
{
    size_t committedSize = Heap::GetHeap().GetAllocatedSize();
    size_t trueLive = cycleStats_.trueLiveBytes;
    size_t heapSize = Heap::GetHeap().GetUsedPageSize();
    double utilization = (committedSize == 0) ? 0 : (static_cast<double>(trueLive) / committedSize) * 100;

    VLOG(REPORT, "[MS-GC] allocated size: %s, heap size: %s, heap utilization: %.2f%%",
         Pretty(committedSize).Str(), Pretty(heapSize).Str(), utilization);
    VLOG(REPORT, "[MS-GC] live bytes %s, marked %zu objects",
         Pretty(trueLive).Str(), markedObjectCount.load(std::memory_order_relaxed));
    VLOG(REPORT, "[MS-GC] total gc time: %s us (MarkSetup %s + ConcMark %s + MarkTerm %s + PostMark %s), sweep pending",
         Pretty(gcTimeUs).Str(),
         Pretty(cycleStats_.markSetupUs).Str(),
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         Pretty(cycleStats_.markTerminationUs).Str(),
         Pretty(cycleStats_.postMarkUs).Str());

    // Also output the standard LOG(RTLOG_INFO) summary for compatibility
    LOG(RTLOG_INFO,
        "[MS-GC] GC cycle=%llu: collected %zu objects, %.2f%% utilization (%s/%s), gc time: %sus",
        static_cast<unsigned long long>(gcCycle_),
        markedObjectCount.load(std::memory_order_relaxed),
        utilization,
        Pretty(trueLive).Str(), Pretty(heapSize).Str(),
        Pretty(gcTimeUs).Str());
}

// ============================================================================
// MS-GC specific UpdateGCStats: simplified Go-style threshold
// ============================================================================
void MarkSweepCollector::UpdateGCStats()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    GCStats& gcStats = GetGCStats();
    gcStats.Dump();

    size_t oldThreshold = gcStats.heapThreshold;
    // Use true live bytes (computed from markBitmaps) instead of AllocatedBytes()
    // which returns totalMSRegionBytes_ (committed region bytes, only increases).
    size_t liveBytes = cycleStats_.trueLiveBytes;
    size_t committedBytes = space.AllocatedBytes();
    size_t heapSize = space.GetMaxCapacity();
    size_t recentBytes = space.GetRecentAllocatedSize();

    // 2 / 3: when committed bytes is over 2/3 heap size, the async allocation need to be closed.
    if (committedBytes > heapSize * 2 / 3) {
        space.EnableAsyncAllocation(false);
    } else {
        space.EnableAsyncAllocation(true);
    }

    // Simplified Go-style threshold: next GC triggers at liveBytes * (1 + heapGrowth).
    double heapGrowth = 1 + (CangjieRuntime::GetHeapParam().heapGrowth);
    size_t newThreshold = static_cast<size_t>(liveBytes * heapGrowth);
    uint64_t gcInterval = CangjieRuntime::GetGCParam().gcInterval;

    // 0.98: make sure new threshold does not exceed reasonable limit.
    newThreshold = std::min(newThreshold, static_cast<size_t>(heapSize * 0.98));
    gcStats.heapThreshold = std::min(newThreshold, CangjieRuntime::GetGCParam().gcThreshold);
    g_gcRequests[GC_REASON_HEU].SetMinInterval(gcInterval);
    VLOG(REPORT, "[MS-GC] live bytes %zu (survived %zu, recent-allocated %zu), update gc threshold %zu -> %zu",
         liveBytes, liveBytes - recentBytes, recentBytes, oldThreshold, gcStats.heapThreshold);
    VLOG(REPORT, "[MS-GC]   committed %s B, true live %s B (%.1f%% utilization)",
         Pretty(committedBytes).Str(), Pretty(liveBytes).Str(),
         (committedBytes == 0) ? 0.0 : (static_cast<double>(liveBytes) / committedBytes) * 100);
    TRACE_COUNT("CJRT_post_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
