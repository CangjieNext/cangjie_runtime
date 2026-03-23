// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MarkSweepCollector.h"
#include <cstring>
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
#include "Mutator/SatbBuffer.h"

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
//   TL-region flush is deferred to ConcurrentSweep's STW (FlushAll +
//   PrepareForSweep), because marking is address-based and does not
//   require regions to be in CentralRegionPool's lists.
// ============================================================================

void MarkSweepCollector::PreEnumRoots()
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- PreEnumRoots ---");

    // Set activeCollector_ for MSMarkAssist (Mutator assist during concurrent mark).
    activeCollector_.store(this, std::memory_order_release);

    // Initialize SatbBuffer before concurrent marking begins.
    SatbBuffer::Instance().Init();

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
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

    cycleStats_.forceFinishSweepUs = TimeUtil::MicroSeconds() - t0;
    VLOG(REPORT, "[MS-GC] PreEnumRoots: %sus",
         Pretty(cycleStats_.forceFinishSweepUs).Str());
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


void MarkSweepCollector::EnumAndMarkRoots(WorkStack& workStack)
{
    MRT_PHASE_TIMER("MS EnumAndMarkRoots");

    // MS-GC: export roots need no special handling (no fromSpace/toSpace
    // pointer update), so collect all roots into a single workStack.
    DoEnumeration(workStack, workStack);
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

        // Mark on pop (same as CopyGC's ConcurrentMarkingWork::Execute):
        // if already marked, skip — otherwise mark and trace children.
        if (MarkObject(obj)) {
            continue;  // already marked by another thread or earlier pass
        }
        nNewlyMarked++;
        processed++;

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

        // Trace all reference fields: push children as grey (mark-on-pop)
        TraceObjectRefFields(obj, workStack);
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
    if (!IsMarkedObject(target)) {
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
    // Legacy stub — was used by MSWriteBarrier, now using SatbBuffer path.
    (void)wbBuf;
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

    size_t nNewlyMarked = 0;
    while (nNewlyMarked < kAssistMaxObjectsPerCall && !localWork.empty()) {
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

        // Mark on pop (same pattern as RunCollectorDrainSlice)
        if (MarkObject(obj)) {
            continue;  // already marked
        }
        nNewlyMarked++;

        if (!obj->HasRefField()) {
            continue;
        }

        // WeakRef: defer to GC main thread (WeakRefBuffer may not be thread-safe)
        if (UNLIKELY(obj->IsWeakRef())) {
            PushSingleToGlobal(obj);
            continue;
        }

        // Trace children → push grey objects to localWork (mark-on-pop)
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

    // Batch update: one atomic op per assist call instead of per-object
    (void)markedObjectCount.fetch_add(nNewlyMarked, std::memory_order_relaxed);
    assistMarkedCount_.fetch_add(nNewlyMarked, std::memory_order_relaxed);
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

    // 1. Drain SatbBuffer: collect from per-Mutator satbNodes + global retiredNodes
    {
        uint64_t t0 = TimeUtil::MicroSeconds();
        DrainSatbBufferToMainQueue(mainQueue);
        cycleStats_.wbBufFlushUs = TimeUtil::MicroSeconds() - t0;
        cycleStats_.wbBufEntriesTotal = mainQueue.size();
    }

    // 1b. Drain global work queue — objects pushed during concurrent mark
    //     (via ShareWorkToGlobal / PushToGlobal) that weren't consumed.
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
    VLOG(REPORT, "[MS-GC] DrainSatbBuffer (final): %zu entries, %sus",
         cycleStats_.wbBufEntriesTotal, Pretty(cycleStats_.wbBufFlushUs).Str());
    VLOG(REPORT, "[MS-GC] MarkTermination total: %sus, total marked %zu",
         Pretty(cycleStats_.markTerminationUs).Str(),
         markedObjectCount.load(std::memory_order_relaxed));

    // Transition to MS_SWEEP INSIDE STW so that when mutators resume,
    // the phase is MS_SWEEP (idle barrier). This eliminates a gap where
    // mutators could trigger the SATB barrier after MarkTermination drain
    // but before PostMarkTermination switches the phase.
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_SWEEP, false);

    // Clear SatbBuffer: return all retired nodes to free pool
    SatbBuffer::Instance().ClearBuffer();
}

void MarkSweepCollector::CollectUnmarkedToMainQueue(WorkStack& mainQueue)
{
    // Drain SatbBuffer (replaces FlushAllWriteBarrierBuffers)
    DrainSatbBufferToMainQueue(mainQueue);

    // Collect extern (foreign) object candidates
    CollectExternCandidatesToMain(mainQueue);

    // Collect finalizer resurrection candidates
    CollectResurrectionCandidatesToMain(mainQueue);
}

void MarkSweepCollector::DrainSatbBufferToMainQueue(WorkStack& mainQueue)
{
    // Under STW: collect objects from all Mutators' satbNodes and global retiredNodes.
    // Filter: only keep heap objects that are not yet marked (need tracing).
    WorkStack remarkStack;

    // 1. Collect from each Mutator's active satbNode
    auto func = [&remarkStack](Mutator& mutator) {
        const SatbBuffer::Node* node = mutator.GetSatbBufferNode();
        if (node != nullptr) {
            const_cast<SatbBuffer::Node*>(node)->GetObjects(remarkStack);
        }
    };
    MutatorManager::Instance().VisitAllMutators(func);

    // 2. Collect from global retiredNodes
    SatbBuffer::Instance().GetRetiredObjects(remarkStack);

    // 3. Filter: only keep valid heap objects not yet marked
    while (!remarkStack.empty()) {
        BaseObject* obj = remarkStack.back();
        remarkStack.pop_back();
        if (obj != nullptr && IsManagedAddress(reinterpret_cast<MAddress>(obj)) &&
            obj->IsValidObject() && !IsMarkedObject(obj)) {
            mainQueue.push_back(obj);
        }
    }
}

void MarkSweepCollector::FlushAllWriteBarrierBuffers(WorkStack& mainQueue)
{
    // Legacy stub — now using SatbBuffer path. Kept for interface compatibility.
    (void)mainQueue;
}

void MarkSweepCollector::ClearAllWriteBarrierBuffers()
{
    // Legacy stub — now using SatbBuffer path. Kept for interface compatibility.
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
    size_t initialSize = mainQueue.size();
    VLOG(REPORT, "[MS-GC] DrainMainQueue start: %zu items", initialSize);

    while (!mainQueue.empty()) {
        BaseObject* obj = mainQueue.back();
        mainQueue.pop_back();
        processed++;

        if ((processed % 100000) == 0 || initialSize <= 20) {
            VLOG(REPORT, "[MS-GC] DrainQ item#%zu: obj=%p managed=%d qRemain=%zu",
                 processed, (void*)obj,
                 (obj != nullptr ? (int)IsManagedAddress(reinterpret_cast<MAddress>(obj)) : -1),
                 mainQueue.size());
        }

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

        // Try to mark. If newly marked, count it.
        // But ALWAYS trace children — objects from the write barrier buffer
        // are already marked by MarkAndEnqueue; skipping them would leave
        // their children untraced and subject to incorrect sweeping.
        bool alreadyMarked = MarkObject(obj);
        if (!alreadyMarked) {
            nNewlyMarked++;
        }

        if (initialSize <= 20) {
            VLOG(REPORT, "[MS-GC] DrainQ item#%zu: marked=%d hasRef=%d qSize=%zu",
                 processed, (int)alreadyMarked, (int)obj->HasRefField(), mainQueue.size());
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

    VLOG(REPORT, "[MS-GC] DrainMainQueue done: %zu processed (initial %zu)", processed, initialSize);
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

    // 0.98: make sure new threshold does not exceed reasonable limit.
    newThreshold = std::min(newThreshold, static_cast<size_t>(heapSize * 0.98));
    gcStats.heapThreshold = std::min(newThreshold, CangjieRuntime::GetGCParam().gcThreshold);
    // No minimum GC interval — trigger immediately when threshold is reached.
    g_gcRequests[GC_REASON_HEU].SetMinInterval(0);
    VLOG(REPORT, "[MS-GC] live bytes %zu (survived %zu, recent-allocated %zu), update gc threshold %zu -> %zu",
         liveBytes, liveBytes - recentBytes, recentBytes, oldThreshold, gcStats.heapThreshold);
    VLOG(REPORT, "[MS-GC]   committed %s B, true live %s B (%.1f%% utilization)",
         Pretty(committedBytes).Str(), Pretty(liveBytes).Str(),
         (committedBytes == 0) ? 0.0 : (static_cast<double>(liveBytes) / committedBytes) * 100);
    TRACE_COUNT("CJRT_post_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
