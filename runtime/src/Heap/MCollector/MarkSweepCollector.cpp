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
#include <string>
#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "CangjieRuntime.h"
#include "Common/GCRefFieldScan.inline.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSRegionManager.h"
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Heap/MCollector/MSHeapThresholdPolicy.h"
#include "Heap/MCollector/MSMarkQueuePolicy.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Collector/ForwardDataManager.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"
#include "TypeInfoManager.h"

#ifndef _WIN32
#include <unistd.h>  // usleep
#endif

namespace MapleRuntime {

namespace {
size_t ComputeScavengeThresholdBytes(size_t heapThresholdBytes, double scavengeGrowth)
{
    if (heapThresholdBytes == 0) {
        return 0;
    }
    if (scavengeGrowth <= 0.0) {
        return heapThresholdBytes;
    }
    long double scaled = static_cast<long double>(heapThresholdBytes) * static_cast<long double>(scavengeGrowth);
    if (scaled >= static_cast<long double>(std::numeric_limits<size_t>::max())) {
        return std::numeric_limits<size_t>::max();
    }
    return static_cast<size_t>(scaled);
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

static const char* GetDiagTypeName(const BaseObject* obj)
{
    if (obj == nullptr || obj->GetTypeInfo() == nullptr || obj->GetTypeInfo()->GetName() == nullptr) {
        return "<null>";
    }
    return obj->GetTypeInfo()->GetName();
}

static uintptr_t LoadStateWordForDiag(const BaseObject* obj)
{
    return obj == nullptr ? 0 : *reinterpret_cast<const uintptr_t*>(obj);
}

static bool IsInterestingDiagType(const BaseObject* obj)
{
    const char* name = GetDiagTypeName(obj);
    return std::strstr(name, "HashMap") != nullptr ||
           std::strstr(name, "HashMapEntry") != nullptr ||
           std::strstr(name, "ArrayList") != nullptr ||
           std::strstr(name, "Timer") != nullptr ||
           std::strstr(name, "RawArray") != nullptr;
}

static bool IsVerifyDetailEnabled()
{
    return CangjieRuntime::GetGCParam().msVerifyMarkDetail != 0;
}

static bool IsVerifyMarkEnabled()
{
    return CangjieRuntime::GetGCParam().msVerifyMark != 0;
}

static std::string HexForDiag(uintptr_t value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%zx", value);
    return std::string(buf);
}

static std::string DescribeTypeLayoutForDiag(TypeInfo* ti)
{
    if (ti == nullptr) {
        return "type=<null>";
    }

    TypeGCInfo gcInfo;
    CString gcTibStr = gcInfo.GetGCTibStr(ti);
    std::string desc = "type=";
    desc += (ti->GetName() != nullptr) ? ti->GetName() : "<?>";
    desc += " kind=" + std::to_string(static_cast<int>(ti->GetType()));
    desc += " inst=" + std::to_string(static_cast<size_t>(ti->GetInstanceSize()));
    desc += " align=" + std::to_string(static_cast<size_t>(ti->GetAlign()));
    desc += " hasRef=" + std::to_string(static_cast<int>(ti->HasRefField()));
    desc += " fieldNum=" + std::to_string(static_cast<unsigned>(ti->GetFieldNum()));
    desc += " gctib=" + std::string(gcTibStr.Str());

    if (ti->IsRawArray()) {
        desc += " component={";
        desc += DescribeTypeLayoutForDiag(ti->GetComponentTypeInfo());
        desc += "}";
        return desc;
    }

    if (ti->GetFieldNum() == 0) {
        return desc;
    }

    desc += " fields=[";
    U32* offsets = ti->GetFieldOffsets();
    for (U16 i = 0; i < ti->GetFieldNum(); ++i) {
        if (i != 0) {
            desc += ", ";
        }
        TypeInfo* fieldTi = ti->GetFieldType(i);
        desc += std::to_string(static_cast<unsigned>(i));
        desc += ":off=" + std::to_string(static_cast<unsigned>(offsets[i]));
        desc += ":";
        desc += (fieldTi != nullptr && fieldTi->GetName() != nullptr) ? fieldTi->GetName() : "<?>";
    }
    desc += "]";
    return desc;
}

static std::string DescribeSlotForDiag(BaseObject* obj, RefField<>& field)
{
    uintptr_t objAddr = reinterpret_cast<uintptr_t>(obj);
    uintptr_t slotAddr = reinterpret_cast<uintptr_t>(&field);
    std::string desc = "slotAddr=" + HexForDiag(slotAddr) +
                       " slotObjOff=" + std::to_string(static_cast<size_t>(slotAddr - objAddr));

    TypeInfo* ti = obj != nullptr ? obj->GetTypeInfo() : nullptr;
    if (ti == nullptr) {
        return desc;
    }

    if (ti->IsRawArray()) {
        MArray* array = reinterpret_cast<MArray*>(obj);
        uintptr_t contentAddr = objAddr + MArray::GetContentOffset();
        uintptr_t contentOff = slotAddr - contentAddr;
        TypeInfo* componentTi = array->GetComponentTypeInfo();
        desc += " contentOff=" + std::to_string(static_cast<size_t>(contentOff));
        if (componentTi != nullptr && componentTi->IsStructType()) {
            size_t elemSize = array->GetElementSize();
            size_t elemIndex = elemSize == 0 ? 0 : static_cast<size_t>(contentOff / elemSize);
            size_t elemOff = elemSize == 0 ? 0 : static_cast<size_t>(contentOff % elemSize);
            desc += " elemIndex=" + std::to_string(elemIndex);
            desc += " elemOff=" + std::to_string(elemOff);
            U32* offsets = componentTi->GetFieldOffsets();
            for (U16 i = 0; i < componentTi->GetFieldNum(); ++i) {
                if (offsets[i] != elemOff) {
                    continue;
                }
                TypeInfo* fieldTi = componentTi->GetFieldType(i);
                desc += " field=" + std::to_string(static_cast<unsigned>(i));
                desc += ":";
                desc += (fieldTi != nullptr && fieldTi->GetName() != nullptr) ? fieldTi->GetName() : "<?>";
                break;
            }
        } else {
            desc += " elemIndex=" +
                    std::to_string(static_cast<size_t>(contentOff / sizeof(RefField<>)));
        }
        return desc;
    }

    uintptr_t payloadAddr = objAddr + TYPEINFO_PTR_SIZE;
    uintptr_t payloadOff = slotAddr - payloadAddr;
    desc += " payloadOff=" + std::to_string(static_cast<size_t>(payloadOff));
    U32* offsets = ti->GetFieldOffsets();
    for (U16 i = 0; i < ti->GetFieldNum(); ++i) {
        if (offsets[i] != payloadOff) {
            continue;
        }
        TypeInfo* fieldTi = ti->GetFieldType(i);
        desc += " field=" + std::to_string(static_cast<unsigned>(i));
        desc += ":";
        desc += (fieldTi != nullptr && fieldTi->GetName() != nullptr) ? fieldTi->GetName() : "<?>";
        break;
    }
    return desc;
}

// Helper: GC tracing paths only treat main-heap addresses as managed.
static inline bool IsManagedAddress(MAddress addr)
{
    return Heap::IsHeapAddress(addr);
}

static inline bool IsCurrentSweepGenObject(BaseObject* obj, uint32_t currentSweepGen)
{
    if (obj == nullptr) {
        return false;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (!MSRegionInfoArray::IsHeapAddress(addr)) {
        return false;
    }

    MSRegionInfo* msInfo = MSRegionInfoArray::GetRegionInfoValidated(addr);
    if (msInfo == nullptr) {
        return false;
    }

    return msInfo->GetSweepGen() == currentSweepGen;
}

// Validate that `obj` is a live traceable heap object, and on success also hand
// back the resolved MSRegionInfo* so the caller can mark without re-resolving
// the region (see MarkObjectInHeapKnown). outInfo is set only when returning
// true; it is the same pointer GetRegionInfoValidated(addr) yields.
static inline bool IsTraceableObjectAddressEx(BaseObject* obj, MSRegionInfo*& outInfo)
{
    outInfo = nullptr;
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
        if (base->allocPtr == addr && (addr + sizeof(uintptr_t) <= base->regionEnd)) {
            outInfo = msInfo;
            return true;
        }
        return false;
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
    size_t slotIdx = (addr - regionStart) / slotSize;
    if (slotIdx >= msInfo->totalSlotCount) {
        return false;
    }
    if (msInfo->regionType == MSRegionType::MS_REGION_BUMP && addr >= msInfo->allocPtr) {
        return false;
    }

    outInfo = msInfo;
    return true;
}

static inline bool IsTraceableObjectAddress(BaseObject* obj)
{
    MSRegionInfo* unused = nullptr;
    return IsTraceableObjectAddressEx(obj, unused);
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
    // MSGC finishes SATB-independent cleanup in MarkTermination/PostMarkTermination.
    // After UpdateGCStats, the remaining hook work is only the collector-finished
    // publication and the final return to the idle barrier state.
    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    collectorResources.NotifyGCFinished(gcIndex);

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    DumpAfterGC();
#endif
    cycleStats_.postGcHookUs = TimeUtil::MicroSeconds() - t0;
}

// ============================================================================
// GC entry point
// ============================================================================

bool MarkSweepCollector::ShouldExecuteQueuedRequest(GCReason reason)
{
    if (reason != GC_REASON_HEU) {
        return true;
    }

    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool == nullptr) {
        return true;
    }

    size_t sweptBytes = space.GetSweptHeapBytes();
    size_t threshold = GetGCStats().GetThreshold();
    SweepDebtDecision debt = centralPool->RefreshSweepDebtSnapshot(threshold, sweptBytes);
    MSHeuExecutionDecision decision = EvaluateMSHeuExecution(sweptBytes, threshold, debt);
    if (decision == MSHeuExecutionDecision::RUN) {
        return true;
    }

    if (decision == MSHeuExecutionDecision::DEFER_FOR_SWEEP) {
        BackgroundProcessor& backgroundProcessor =
            Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
        backgroundProcessor.NotifyToParallelSweep(DeferredHeuOrigin::EXECUTE_SIDE);
    }
    VLOG(REPORT,
         "[MS-GC] queued HEU skipped before execute: swept=%zuB threshold=%zuB mode=%d decision=%d",
         sweptBytes, threshold, static_cast<int>(debt.mode), static_cast<int>(decision));
    return false;
}

void MarkSweepCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    gcCycle_++;
    cycleStats_.Reset();
    wbFlushStats_.Reset();
    SetGCReason(reason);

    GCStats& gcStats = GetGCStats();
    gcStats.reason = reason;
    gcStats.collectedBytes = 0;
    gcStats.gcStartTime = TimeUtil::NanoSeconds();
    // Block ①: GC Entry
    LogGCEntry(gcIndex, reason);

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
// Step 2.1a: PreEnumRoots  (GC thread only)
//   Stop all sweeps, wait for in-flight bg/lazy sweep to quiesce, clear
//   markBitmaps, then briefly stop the world for reclassify + generation flip
//   that freezes this cycle's sweep snapshot.
//
// ============================================================================

void MarkSweepCollector::PreEnumRoots()
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- PreEnumRoots ---");

    // Set activeCollector_ for MSMarkAssist (Mutator assist during concurrent mark).
    activeCollector_.store(this, std::memory_order_release);

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    
    // Stop sweep → wait for in-flight sweep to quiesce → under STW clear
    // bitmaps / reclassify residual unswept entries / flip sweep generation
    // BEFORE concurrent mark begins. Flipping the generation here (rather than
    // in MarkTermination) guarantees that every region allocated during this
    // GC cycle's mark phase carries the NEW sweep generation, so the current
    // sweep skips them. Otherwise such regions keep the OLD generation and get
    // swept this cycle, which wrongly reclaims live objects that are only held
    // by stack/local roots (those never pass through the write barrier).
    // Safe pre-mark: no SweepRegion running, mark-at-alloc inactive.
    if (centralPool != nullptr) {
        uint64_t stopSweepT0 = TimeUtil::MicroSeconds();
        centralPool->StopAndWaitForAllSweep();
        uint64_t stopSweepUs = TimeUtil::MicroSeconds() - stopSweepT0;

        uint64_t clearBitmapUs = 0;
        uint64_t stwT0 = TimeUtil::MicroSeconds();
        {
            ScopedStopTheWorld stw("MS PreEnumRoots clear/reclassify/prepare STW");
            uint64_t clearBitmapT0 = TimeUtil::MicroSeconds();
            centralPool->ClearAllMarkBitmaps();
            clearBitmapUs = TimeUtil::MicroSeconds() - clearBitmapT0;
            centralPool->ReclassifyUnsweptToSwept();
            centralPool->PrepareForSweep();
        }
        uint64_t stwUs = TimeUtil::MicroSeconds() - stwT0;
        VLOG(REPORT,
             "[MS-GC] PreEnumRoots: stage-times stopSweep=%sus clearBitmaps=%sus stw=%sus",
             Pretty(stopSweepUs).Str(),
             Pretty(clearBitmapUs).Str(),
             Pretty(stwUs).Str());
    }
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

bool MarkSweepCollector::DiscoverObjectNoCount(BaseObject* obj)
{
    // Guard against stale pointers to reclaimed/zeroed slots.
    // For such slots stateWord->TypeInfo is null, and they must not be enqueued
    // into mark work queues in concurrent mark.
    // Resolve the region once here and reuse it for the mark step
    // (MarkObjectInHeapKnown) instead of letting MarkObject re-resolve it.
    MSRegionInfo* msInfo = nullptr;
    if (UNLIKELY(!IsTraceableObjectAddressEx(obj, msInfo) || !obj->IsValidObject())) {
        return false;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (MSRegionInfoArray::MarkObjectInHeapKnown(msInfo, addr)) {
        return false;  // already marked
    }
    return true;
}

bool MarkSweepCollector::ResolveSmallRefForBarrier(BaseObject* ref, MSRegionInfo*& info,
                                                   size_t& wordIdx, uint64_t& bitMask) const
{
    info = nullptr;
    if (UNLIKELY(!IsTraceableObjectAddressEx(ref, info) || !ref->IsValidObject()) ||
        info->regionType == MSRegionType::MS_REGION_LARGE) {
        info = nullptr;
        return false;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(ref);
    size_t slotIdx = SizeClassInfo::OffsetToSlotIndex(info->sizeClass, addr - info->RegionStart());
    if (UNLIKELY(slotIdx >= info->totalSlotCount || info->markBitmap == nullptr)) {
        info = nullptr;
        return false;
    }
    wordIdx = slotIdx / 64;
    bitMask = 1ULL << (slotIdx % 64);
    return true;
}

bool MarkSweepCollector::DiscoverObject(BaseObject* obj)
{
    return DiscoverObjectNoCount(obj);
}

bool MarkSweepCollector::TryEnqueueGreyObject(BaseObject* obj, GreyWorkSink& sink)
{
    bool newlyMarked = DiscoverObject(obj);
    if (ShouldEnqueueMSMarkedObject(newlyMarked, newlyMarked && obj->HasRefField())) {
        sink.PushGrey(obj);
    }
    return newlyMarked;
}

bool MarkSweepCollector::TryEnqueueGreyObject(BaseObject* obj, GreyWorkQueue& queue)
{
    bool newlyMarked = DiscoverObject(obj);
    if (ShouldEnqueueMSMarkedObject(newlyMarked, newlyMarked && obj->HasRefField())) {
        queue.PushNewlyGrey(obj);
    }
    return newlyMarked;
}

void MarkSweepCollector::DiscoverAndPushObject(BaseObject* obj, WorkStack& workStack)
{
    WorkStackGreySink sink(workStack);
    (void)TryEnqueueGreyObject(obj, sink);
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
        centralPool->VisitAllocBufferMS([&mutatorRoots](AllocBufferMS& buffer) {
            buffer.MergeRoots(mutatorRoots);
        });
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

void MarkSweepCollector::ConcurrentMark()
{
    // activeCollector_ was already set in MarkSetup (before STW).

    // 8. Transition to concurrent marking phase
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_CONCURRENT_MARK, true);

    // 9. Reset cycle-global assist state. Per-P diagnostics are reset while
    // initializing each AllocBufferMS below.
    {
        std::lock_guard<std::mutex> lock(exitedAssistStatsMutex_);
        exitedAssistStats_ = MSMarkAssistStats{};
    }
    bgScanCredit_.store(0, std::memory_order_relaxed);
    bgCreditInjected_ = 0;
    peakActiveHelpers_ = 0;
    workerScaleUpCount_ = 0;
    peakPublishedBacklog_ = 0;

    // 10. Init per-P GCWork for all existing mutators before enabling assist.
    //     New mutators that join later will lazily init in MSMarkAssist.
    InitAllPerPGCWork();
    markWorkerControl_.Start(0);

    // 11. Compute this cycle's base ratio from the previous mark density, then
    //     apply cycle-to-cycle rise limiting and assist-share feedback.
    {
        uint64_t baseRatioPercent = CangjieRuntime::GetGCParam().msAssistBaseRatioPercent;
        size_t prevMarked = prevCycleMarkedObjects_.load(std::memory_order_relaxed);
        size_t allocSize = Heap::GetHeap().GetAllocatedSize();
        uint64_t rawRatio = ComputeMSAssistBaseRatioScaled(prevMarked, allocSize, baseRatioPercent);
        MSAssistBaseRatioDecision decision = EvaluateMSAssistBaseRatio(
            rawRatio, previousAssistBaseRatioScaled_, assistBaseRatioInitialized_,
            previousAssistProcessedObjects_, previousBackgroundProcessedObjects_);
        uint64_t initialRatioScaled = decision.nextRatio;
        previousAssistBaseRatioScaled_ = initialRatioScaled;
        if (prevMarked != 0 && allocSize != 0) {
            assistBaseRatioInitialized_ = true;
        }
        assistWorkPerByteScaled_.store(initialRatioScaled, std::memory_order_release);
        VLOG(REPORT,
             "[MS-GC] Assist enabled: ratioScaled=%llu, basePercent=%llu (prevMarked=%zu, allocSize=%zu)",
             static_cast<unsigned long long>(initialRatioScaled),
             static_cast<unsigned long long>(baseRatioPercent),
             prevMarked,
             allocSize);
        VLOG(REPORT,
             "[MS-GC] [assist-pacing] rawBase=%llu prevBase=%llu nextBase=%llu riseLimit=%llu "
             "feedbackLimit=%llu assistShare=%llu.%02llu%% riseLimited=%d shareLimited=%d "
             "prevMainHelper=%zu prevAssist=%zu",
             static_cast<unsigned long long>(decision.rawRatio),
             static_cast<unsigned long long>(decision.previousRatio),
             static_cast<unsigned long long>(decision.nextRatio),
             static_cast<unsigned long long>(decision.riseLimit),
             static_cast<unsigned long long>(decision.feedbackLimit),
             static_cast<unsigned long long>(decision.assistShareBasisPoints / 100),
             static_cast<unsigned long long>(decision.assistShareBasisPoints % 100),
             static_cast<int>(decision.riseLimited),
             static_cast<int>(decision.shareLimited),
             previousBackgroundProcessedObjects_,
             previousAssistProcessedObjects_);
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

    size_t configuredThreads = std::max<int32_t>(1, CangjieRuntime::GetGCParam().gcThreads);
    size_t maxHelpers = configuredThreads > 1 ? configuredThreads - 1 : 0;
    GCThreadPool* threadPool = GetThreadPool();
    if (threadPool == nullptr) {
        maxHelpers = 0;
    } else {
        maxHelpers = std::min(maxHelpers, static_cast<size_t>(threadPool->GetMaxThreadNum()));
    }
    bool markPoolOwned = false;
    if (maxHelpers != 0) {
        markPoolOwned = collectorResources.AcquireMSGCWorkerPoolForMark();
        if (!markPoolOwned) {
            // Runtime shutdown may close the pool owner gate while a queued GC is
            // finishing. Complete the mark serially instead of publishing work
            // into a pool that is entering STOPPING.
            maxHelpers = 0;
        }
    }
    MSMarkWorkerPolicy workerPolicy(maxHelpers);
    MSMarkWorkerDecision workerDecision = workerPolicy.Update(globalFull_.GetCounts().total);
    StartMarkWorkers(maxHelpers, workerDecision.activeHelpers);
    peakActiveHelpers_ = workerDecision.activeHelpers;

    // GC-supply credit with configurable linear decay.
    // Shorter window forces assist to do real marking work sooner.
    const size_t creditDecayRounds = CangjieRuntime::GetGCParam().msAssistCreditDecayRounds;
    const size_t backlogStepBuffers = CangjieRuntime::GetGCParam().msAssistBacklogStepBuffers;
    const size_t maxAssistMultiplier = CangjieRuntime::GetGCParam().msAssistMaxMultiplier;
    size_t roundIdx = 0;
    double smoothedBacklog = 0.0;
    bool backlogEMAInitialized = false;

    // Capture base assist ratio for dynamic escalation below.
    const uint64_t baseAssistRatio = assistWorkPerByteScaled_.load(std::memory_order_acquire);
    const uint64_t tailAssistRatio = ComputeMSAssistTailRatioScaled(baseAssistRatio, maxAssistMultiplier);
    bool tailDraining = false;
    uint64_t tailStartUs = 0;
    size_t quiesceResidualBuffers = 0;

    // Mark time budget: if concurrent marking exceeds this wall-clock budget,
    // force-converge and hand the residual grey work to MarkTermination (STW).
    // Under STW all mutators are frozen, so no new objects are allocated and the
    // remaining work set is bounded and guaranteed to drain — no missed marks.
    // Configurable via cjMSMarkTimeBudget (default 20s;
    // 0 disables the budget and restores pure convergence-based exit).
    uint64_t markBudgetNs = CangjieRuntime::GetGCParam().msMarkTimeBudget;
    const uint64_t markBudgetUs = markBudgetNs == 0 ? 0 : std::max<uint64_t>(1, markBudgetNs / 1000ULL);
    bool forceConverged = false;
    size_t gcMainProcessed = 0;

    while (true) {
        // Every published buffer is one GC-main mark slice, regardless of length.
        size_t scannedCount = DrainOneBufRound(gcThreadWork_, false);
        gcMainProcessed += scannedCount;

        LFStack::Counts backlog = globalFull_.GetCounts();
        size_t publishedBacklog = backlog.total;
        size_t fullBacklog = backlog.full;
        peakPublishedBacklog_ = std::max(peakPublishedBacklog_, publishedBacklog);
        size_t oldHelpers = workerPolicy.GetActiveHelpers();
        workerDecision = workerPolicy.Update(publishedBacklog);
        MRT_ASSERT(workerDecision.activeHelpers >= oldHelpers,
                   "MSGC mark helpers must not scale down within a cycle");
        if (workerDecision.activeHelpers > oldHelpers) {
            markWorkerControl_.SetTargetHelpers(workerDecision.activeHelpers);
            ++workerScaleUpCount_;
            peakActiveHelpers_ = std::max(peakActiveHelpers_, workerDecision.activeHelpers);
        }

        // Dynamic assist ratio escalation based on globalFull_ backpressure:
        // Smooth transient queue changes, then increase continuously up to the configured cap.
        if (tailDraining) {
            assistWorkPerByteScaled_.store(tailAssistRatio, std::memory_order_release);
        } else if (baseAssistRatio > 0) {
            size_t backlog = fullBacklog;
            smoothedBacklog = UpdateMSAssistBacklogEMA(smoothedBacklog, backlog, backlogEMAInitialized);
            backlogEMAInitialized = true;
            uint64_t adjustedRatio = ComputeMSAssistBacklogRatioScaled(
                baseAssistRatio, smoothedBacklog, backlogStepBuffers, maxAssistMultiplier);
            assistWorkPerByteScaled_.store(adjustedRatio, std::memory_order_release);
        }

        // Per-round decayed GC-supply credit injection.
        size_t workerScanned = TakePublishedWorkerCredit();
        size_t backgroundScanned = scannedCount + workerScanned;
        if (roundIdx < creditDecayRounds && backgroundScanned > 0) {
            double w = static_cast<double>(creditDecayRounds - roundIdx) /
                       static_cast<double>(creditDecayRounds);
            size_t inj = static_cast<size_t>(static_cast<double>(backgroundScanned) * w);
            if (inj > 0) {
                bgScanCredit_.fetch_add(static_cast<int64_t>(inj), std::memory_order_release);
                bgCreditInjected_ += inj;
            }
        }

        roundIdx++;

        bool convergenceCandidate = gcThreadWork_.ConcurrentInputEmpty() && globalFull_.Empty() &&
                                    markWorkerControl_.GetWorkersInFlight() == 0 &&
                                    markWorkerControl_.GetAssistsInFlight() == 0;
        if (convergenceCandidate) {
            if (!tailDraining) {
                tailDraining = true;
                tailStartUs = TimeUtil::MicroSeconds();
                size_t oldHelpers = workerPolicy.GetActiveHelpers();
                workerDecision = workerPolicy.EnterTail();
                markWorkerControl_.SetTargetHelpers(workerDecision.activeHelpers);
                if (workerDecision.activeHelpers > oldHelpers) {
                    ++workerScaleUpCount_;
                }
                peakActiveHelpers_ = std::max(peakActiveHelpers_, workerDecision.activeHelpers);
                assistWorkPerByteScaled_.store(tailAssistRatio, std::memory_order_release);
            }
            quiesceResidualBuffers = QuiesceMarkWorkers();
            break;
        }

        // Time-budget guard: force-converge if marking runs too long. Residual
        // grey work remains in globalFull_/per-P gcWork and is fully drained by
        // MarkTermination under STW (bounded work set, no concurrent alloc).
        if (markBudgetUs != 0 && (TimeUtil::MicroSeconds() - t0) >= markBudgetUs) {
            forceConverged = true;
            quiesceResidualBuffers = QuiesceMarkWorkers();
            break;
        }
    }

    cycleStats_.concurrentMarkUs = TimeUtil::MicroSeconds() - t0;
    VLOG(REPORT,
         "[MS-GC] ConcurrentMark time: %sus (forceConverged=%d)",
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         static_cast<int>(forceConverged));
    size_t helperProcessed = 0;
    for (const auto& slot : markWorkerSlots_) {
        helperProcessed += slot->stats.TotalProcessed();
    }
    cycleStats_.gcMainProcessedObjects = gcMainProcessed;
    cycleStats_.helperProcessedObjects = helperProcessed;
    VLOG(REPORT,
         "[MS-GC] [mark-workers] configured=%zu peakActive=%zu processed=%zu "
         "scaleUp=%zu scaleDown=%zu backlogPeak=%zu workersInFlight=%zu assistInFlight=%zu "
         "mainProcessed=%zu",
         maxHelpers + 1,
         peakActiveHelpers_ + 1,
         helperProcessed,
         workerScaleUpCount_,
         static_cast<size_t>(0),
         peakPublishedBacklog_,
         markWorkerControl_.GetWorkersInFlight(),
         markWorkerControl_.GetAssistsInFlight(),
         gcMainProcessed);
    uint64_t tailDurationUs = tailDraining ? TimeUtil::MicroSeconds() - tailStartUs : 0;
    VLOG(REPORT,
         "[MS-GC] [mark-tail] entered=%d duration=%sus refills=%zu assistRatioScaled=%llu "
         "quiesceResidualBuffers=%zu",
         static_cast<int>(tailDraining),
         Pretty(tailDurationUs).Str(),
         static_cast<size_t>(0),
         static_cast<unsigned long long>(tailDraining ? tailAssistRatio : 0),
         quiesceResidualBuffers);
    if (markPoolOwned) {
        collectorResources.ReleaseMSGCWorkerPool(MSGCWorkerPoolOwner::MARK);
    }
}

void MarkSweepCollector::StartMarkWorkers(size_t maxHelpers, size_t initialHelpers)
{
    markWorkerSlots_.clear();
    markWorkerSlots_.reserve(maxHelpers);
    markWorkerControl_.SetTargetHelpers(initialHelpers);
    if (maxHelpers == 0) {
        return;
    }

    GCThreadPool* threadPool = GetThreadPool();
    for (size_t i = 0; i < maxHelpers; ++i) {
        std::unique_ptr<MSMarkWorkerSlot> slot(new (std::nothrow) MSMarkWorkerSlot());
        CHECK_DETAIL(slot != nullptr, "new MSGC mark worker slot failed");
        slot->work.Init(&globalFull_, &workBufPool_);
        markWorkerSlots_.push_back(std::move(slot));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, i](size_t) { RunMarkWorker(i); }));
    }
    threadPool->Start();
}

void MarkSweepCollector::RunMarkWorker(size_t workerId)
{
    MSMarkWorkerSlot& slot = *markWorkerSlots_[workerId];
    while (markWorkerControl_.WaitUntilEligible(workerId)) {
        if (!markWorkerControl_.TryEnterWorker(workerId)) {
            continue;
        }
        size_t scanned = DrainOneBufRound(slot.work, false);
        if (scanned != 0) {
            slot.stats.RecordScanned(scanned);
        }
        if (scanned == 0) {
            slot.stats.PublishPendingCredit();
        }
        markWorkerControl_.ExitWorker();
        if (scanned == 0) {
            markWorkerControl_.BackoffWorker();
        }
    }
    slot.work.Dispose();
}

size_t MarkSweepCollector::TakePublishedWorkerCredit()
{
    size_t total = 0;
    for (const auto& slot : markWorkerSlots_) {
        total += slot->stats.TakePublishedCredit();
    }
    return total;
}

size_t MarkSweepCollector::QuiesceMarkWorkers()
{
    markWorkerControl_.BeginQuiesce();
    markWorkerControl_.WaitForNoInFlight();
    for (const auto& slot : markWorkerSlots_) {
        slot->stats.PublishPendingCredit();
    }
    // Do not publish private partial putCur_ tails here. Under sustained write-
    // barrier traffic, repeatedly closing those tails concurrently can prevent
    // convergence while every new allocation is conservatively marked live.
    // MarkTermination disposes and drains the bounded tails under STW.
    size_t residualBuffers = globalFull_.GetCounts().total;
    markWorkerControl_.Stop();
    if (!markWorkerSlots_.empty()) {
        GetThreadPool()->WaitFinish();
    }
    return residualBuffers;
}

void MarkSweepCollector::DrainMarkWorkStack()
{
    while (!markWorkStack_.empty()) {
        BaseObject* obj = markWorkStack_.back();
        markWorkStack_.pop_back();
        if (UNLIKELY(!IsTraceableObjectAddress(obj))) {
            continue;
        }

        if (UNLIKELY(!obj->IsValidObject())) {
            LOG(RTLOG_FATAL, "[MS-GC] DrainMarkWorkStack: FATAL invalid stateWord obj=%p, "
                "stateWord=0x%zx, markBit=%d",
                (void*)obj,
                *reinterpret_cast<uintptr_t*>(obj),
                (int)IsMarkedObject(obj));
        }

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
    GreyWorkQueue keepForMarkTermination;
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
        // Non-WeakRef deferred items may still be under construction (for
        // example newly-created finalizer objects). Keep them for
        // MarkTermination so the GC main thread scans them under STW after
        // mutator initialization stores have quiesced.
        keepForMarkTermination.PushMovedGrey(obj);
    }
    if (!keepForMarkTermination.empty()) {
        std::lock_guard<std::mutex> lock(deferredMainThreadMutex_);
        deferredMainThreadQueue_.insert(keepForMarkTermination);
    }
    DrainMarkWorkStack();
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

        // Route-B: entries from a raw write-barrier log (needsDiscover) have not
        // been shaded yet — DiscoverObject here; only the mark-CAS winner scans.
        // TryGetObj leaves getCur_ pointing at the buffer obj came from.
        if (gcw.getCur_ != nullptr && gcw.getCur_->needsDiscover) {
            if (!DiscoverObjectNoCount(obj)) {
                continue;
            }
        } else if (UNLIKELY(WorkBuf::kRevalidateTrustedGrey)) {
            if (UNLIKELY(!IsTraceableObjectAddress(obj))) {
                continue;
            }
            if (UNLIKELY(!obj->IsValidObject())) {
                LOG(RTLOG_FATAL, "[MS-GC] DrainGCWork: FATAL invalid stateWord obj=%p, "
                    "stateWord=0x%zx, markBit=%d",
                    (void*)obj,
                    *reinterpret_cast<uintptr_t*>(obj),
                    (int)IsMarkedObject(obj));
            }
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
            bool newlyMarked = DiscoverObject(target);
            if (ShouldEnqueueMSMarkedObject(newlyMarked, newlyMarked && target->HasRefField())) {
                gcw.PutObj(target);
            }
        }
    };
    ForEachRefFieldGCInline(obj, refFunc);
}

// ============================================================================
// TransferRootsToGCWork: pack root objects into WorkBufs and push directly to
// globalFull_, same as activeLog_ overflow does.
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

// Route-B (Step37): the write-barrier log is a WorkBuf pointer (activeLog_) that
// the barrier appends raw old-refs into. Overflow no longer discovers on the
// mutator — it publishes the full buffer wholesale and installs a fresh one.
WorkBuf* MarkSweepCollector::EnsureActiveLog(AllocBufferMS& ab)
{
    WorkBuf* log = ab.GetActiveLog();
    if (log == nullptr) {
        log = ab.AcquireWorkBuf(workBufPool_);
        ab.ActiveLogRef() = log;
    }
    return log;
}

void MarkSweepCollector::PublishAndSwapLog(AllocBufferMS& ab)
{
    WorkBuf* full = ab.GetActiveLog();
    if (full == nullptr || full->Empty()) {
        return;
    }
    // Route-A: entries were already shaded (mark-CAS won) by the barrier, so the
    // buffer holds only grey objects — the consumer scans them directly.
    full->needsDiscover = false;
    // Make every complete log visible to the GC thread and all assists instead
    // of pinning the first four buffers to the owner P.
    size_t published = full->len;
    globalFull_.Push(full);
    wbFlushStats_.Record(MSGCWriteBarrierFlushSource::OVERFLOW_FLUSH, published, published);

    ab.ActiveLogRef() = ab.AcquireWorkBuf(workBufPool_);
}

void MarkSweepCollector::PublishExitedLog(AllocBufferMS& ab)
{
    MergeExitedAssistStats(ab);
    WorkBuf* log = ab.GetActiveLog();
    if (log != nullptr) {
        if (!log->Empty()) {
            // Owner is leaving: push straight to globalFull_ (no local consumer
            // survives). Route-A: entries are already grey, consumer scans them.
            log->needsDiscover = false;
            size_t published = log->len;
            globalFull_.Push(log);
            wbFlushStats_.Record(MSGCWriteBarrierFlushSource::EXITED_OWNER, published, published);
        } else {
            workBufPool_.Put(log);
        }
        ab.ActiveLogRef() = nullptr;
    }
    ab.DrainLogCacheToPool(workBufPool_);
}

void MarkSweepCollector::MergeExitedAssistStats(AllocBufferMS& allocBuffer)
{
    MSMarkAssistStats exited = allocBuffer.GetMarkAssistStats().TakeAndReset();
    std::lock_guard<std::mutex> lock(exitedAssistStatsMutex_);
    exitedAssistStats_.Merge(exited);
}

void MarkSweepCollector::SnapshotAssistStats()
{
    MSMarkAssistStats total;
    {
        std::lock_guard<std::mutex> lock(exitedAssistStatsMutex_);
        total.Merge(exitedAssistStats_.TakeAndReset());
    }

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->VisitAllocBufferMS([&total](AllocBufferMS& buffer) {
            total.Merge(buffer.GetMarkAssistStats().TakeAndReset());
        });
    }

    cycleStats_.assistCallCount = total.calls;
    cycleStats_.assistProcessedObjects = total.processed;
    cycleStats_.assistGlobalPaid = total.globalPaid;
    cycleStats_.bgScanCreditConsumed = total.bgStolen;
    VLOG(REPORT,
         "[MS-GC] [assist] calls=%zu bgStolen=%zu globalPaid=%zu processed=%zu "
         "creditInjected=%zu",
         total.calls,
         total.bgStolen,
         total.globalPaid,
         total.processed,
         bgCreditInjected_);
}

void MarkSweepCollector::PushSingleToDeferredMainThread(BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    if (UNLIKELY(!IsTraceableObjectAddress(obj))) {
        return;
    }
    if (UNLIKELY(!obj->IsValidObject())) {
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
        // Route-B: a raw write-barrier log (needsDiscover) holds un-shaded refs —
        // DiscoverObject each and only enqueue the newly-grey ones. Already-grey
        // bufs (mark output) are pushed straight through.
        if (buf->needsDiscover) {
            for (size_t i = 0; i < buf->len; i++) {
                BaseObject* obj = buf->objs[i];
                bool newlyMarked = DiscoverObjectNoCount(obj);
                if (ShouldEnqueueMSMarkedObject(newlyMarked, newlyMarked && obj->HasRefField())) {
                    mainQueue.PushMovedGrey(obj);
                }
            }
        } else {
            for (size_t i = 0; i < buf->len; i++) {
                BaseObject* obj = buf->objs[i];
                if (UNLIKELY(WorkBuf::kRevalidateTrustedGrey)) {
                    if (obj == nullptr) {
                        continue;
                    }
                    if (UNLIKELY(!IsTraceableObjectAddress(obj))) {
                        continue;
                    }
                    if (UNLIKELY(!obj->IsValidObject())) {
                        continue;
                    }
                }
                mainQueue.PushMovedGrey(obj);
            }
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
    return static_cast<size_t>(stolen);
}

void MarkSweepCollector::InitAllPerPGCWork()
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->VisitAllocBufferMS([this](AllocBufferMS& buf) {
            buf.GetMarkAssistStats() = MSMarkAssistStats{};
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
    for (const auto& slot : markWorkerSlots_) {
        if (slot->work.IsInitialized()) {
            slot->work.Dispose();
        }
    }
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
    for (const auto& slot : markWorkerSlots_) {
        if (slot->work.IsInitialized()) {
            slot->work.Reset();
        }
    }
    markWorkerSlots_.clear();
}

// ============================================================================
// DrainOneBufRound: acquire and drain exactly one published buffer. GC
// main/helpers accept partial buffers; mutator assist remains full-only.
// Children go to private putCur_ until full or a convergence handshake.
// ============================================================================
size_t MarkSweepCollector::DrainOneBufRound(PerPGCWork& gcw, bool requireFull)
{
    bool acquired = requireFull ? gcw.AcquireOneFullBuffer() : gcw.AcquireOneBuffer();
    if (!acquired) {
        return 0;
    }

    size_t nProcessed = 0;
    // getCur_ is fixed for the whole round, so its raw-log flag is constant here.
    const bool needsDiscover = (gcw.getCur_ != nullptr) && gcw.getCur_->needsDiscover;
    while (gcw.getCur_ != nullptr && !gcw.getCur_->Empty()) {
        BaseObject* obj = gcw.getCur_->Pop();
        if (needsDiscover) {
            // Route-B raw log: shade first; only the mark-CAS winner scans.
            if (!DiscoverObjectNoCount(obj)) {
                continue;
            }
        } else if (UNLIKELY(WorkBuf::kRevalidateTrustedGrey)) {
            if (UNLIKELY(!IsTraceableObjectAddress(obj))) {
                continue;
            }
            if (UNLIKELY(!obj->IsValidObject())) {
                LOG(RTLOG_FATAL, "[MS-GC] DrainOneBufRound: FATAL invalid stateWord obj=%p, "
                    "stateWord=0x%zx, markBit=%d",
                    (void*)obj,
                    *reinterpret_cast<uintptr_t*>(obj),
                    (int)IsMarkedObject(obj));
            }
        }
        nProcessed++;
        if (!obj->HasRefField()) {
            continue;
        }
        if (UNLIKELY(obj->IsWeakRef())) {
            PushSingleToDeferredMainThread(obj);
            continue;
        }
        TraceObjectRefFieldsToGCWork(obj, gcw);
    }
    // Never cascade into partial putCur_ in the same slice.
    if (gcw.getCur_ != nullptr) {
        gcw.workBufPool_->Put(gcw.getCur_);
        gcw.getCur_ = nullptr;
    }
    return nProcessed;
}

// ============================================================================
// MSMarkAssist: preserve allocation-debt pacing, but consume at most one full
// global buffer per call. Partial mark output remains in putCur_ until full/STW.
// ============================================================================
size_t MarkSweepCollector::MSMarkAssist(AllocBufferMS& allocBuffer, size_t debtObjects)
{
    if (debtObjects == 0) {
        return 0;
    }

    if (!markWorkerControl_.TryEnterAssist()) {
        return 0;
    }

    MSMarkAssistStats& stats = allocBuffer.GetMarkAssistStats();
    stats.calls++;
    PerPGCWork& gcw = allocBuffer.GetGCWork();
    if (!gcw.IsInitialized()) {
        gcw.Init(&globalFull_, &workBufPool_);
    }

    // ① Steal GC-supplied credit.
    size_t paidByBg = TryStealBgCreditPartial(debtObjects);
    stats.bgStolen += paidByBg;

    // ② One full global buffer pays exactly one buffer of remaining debt.
    size_t remainDebt = (debtObjects > paidByBg) ? (debtObjects - paidByBg) : 0;
    size_t paidByGlobal = 0;
    if (remainDebt >= WorkBuf::kCapacity) {
        size_t nDrained = DrainOneBufRound(gcw, true);
        if (nDrained > 0) {
            paidByGlobal = WorkBuf::kCapacity;
            stats.globalPaid += paidByGlobal;
            stats.processed += nDrained;
        }
    }

    markWorkerControl_.ExitAssist();
    return paidByBg + paidByGlobal;
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

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    SnapshotAssistStats();

    GreyWorkQueue mainQueue;
    size_t pendingSourceEntries = 0;
    size_t resurrectionEntries = 0;
    MSGCWriteBarrierFlushSnapshot wbFlushSnapshot;

    auto collectPendingMarkWork = [this, &mainQueue]() -> size_t {
        size_t before = mainQueue.size();
        FlushAllWriteBarrierBuffers(mainQueue);
        DisposeAllPerPGCWork();
        DrainGlobalLFStackToMainQueue(mainQueue);
        DrainDeferredMainThreadQueueToMainQueue(mainQueue);
        return mainQueue.size() - before;
    };

    // 1. Collect pending mutator/barrier/concurrent-mark work. Some paths can
    // publish grey work very close to the STW boundary, so MarkTermination drains
    // all sources to a fixed point instead of relying on a single snapshot.
    pendingSourceEntries += collectPendingMarkWork();

    // 2. Collect resurrection/finalizer-related candidates.
    {
        CollectResurrectionCandidatesToMain(mainQueue);
        resurrectionEntries = mainQueue.size() - pendingSourceEntries;
    }

    // 3. Drain main queue with Execute-like loop (same as concurrent mark logic)
    // and re-check all external work sources until no delayed grey work remains.
    {
        cycleStats_.markTermProcessedObjects = 0;
        size_t lateSourceEntries = 0;
        size_t drainRounds = 0;
        do {
            drainRounds++;
            ExecuteLikeDrainMainQueue(mainQueue);
            size_t added = collectPendingMarkWork();
            lateSourceEntries += added;
            pendingSourceEntries += added;
        } while (!mainQueue.empty());
        wbFlushSnapshot = wbFlushStats_.Snapshot();
        if (lateSourceEntries != 0 || drainRounds > 1) {
            VLOG(REPORT,
                 "[MS-GC] MarkTermination fixed-point drain: rounds=%zu lateEntries=%zu",
                 drainRounds,
                 lateSourceEntries);
        }
    }

    // 4b. Diagnostic: marking is now complete and all mutators are frozen
    // (STW). This is the authoritative window to verify mark completeness —
    // no concurrent mutation can produce false positives. No-op unless the
    // env var cjMSVerifyMark is set.
    VerifyMarkCompleteness();

    // 5. Reset all per-P gcWork and return WorkBufs to pool.
    // NOTE: sweep generation is flipped in PreEnumRoots (before mark), NOT
    // here. Flipping after mark would let regions allocated during this
    // cycle's mark phase keep the old generation and be swept this cycle,
    // wrongly reclaiming live stack-only objects (not covered by the barrier).
    ResetAllPerPGCWork();

    cycleStats_.markTerminationUs = TimeUtil::MicroSeconds() - phaseStart;
    VLOG(REPORT,
         "[MS-GC] WriteBarrierFlush: forwarded=%zu objects (overflow=%zu, exit=%zu, final=%zu), "
         "raw=%zu entries",
         wbFlushSnapshot.TotalForwardedObjects(),
         wbFlushSnapshot.forwardedObjects[static_cast<size_t>(MSGCWriteBarrierFlushSource::OVERFLOW_FLUSH)],
         wbFlushSnapshot.forwardedObjects[static_cast<size_t>(MSGCWriteBarrierFlushSource::EXITED_OWNER)],
         wbFlushSnapshot.forwardedObjects[static_cast<size_t>(MSGCWriteBarrierFlushSource::FINAL_STW)],
         wbFlushSnapshot.TotalRawEntries());
    VLOG(REPORT,
         "[MS-GC] MarkTermination total: %sus, flushed=%zu entries, resurrection=%zu entries, "
         "processed=%zu objects",
         Pretty(cycleStats_.markTerminationUs).Str(),
         pendingSourceEntries,
         resurrectionEntries,
         cycleStats_.markTermProcessedObjects);

    // Transition to MS_SWEEP INSIDE STW so that when mutators resume,
    // the phase is MS_SWEEP (idle barrier). This eliminates a gap where
    // mutators could still buffer write-barrier entries after the last drain
    // but before PostMarkTermination switches the phase.
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_SWEEP, false);

    if (centralPool != nullptr) {
        centralPool->ReenableLazySweep();
    }
}

void MarkSweepCollector::FlushAllWriteBarrierBuffers(GreyWorkQueue& mainQueue)
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool == nullptr) {
        return;
    }

    centralPool->VisitAllocBufferMS([this, &mainQueue](AllocBufferMS& buffer) {
        // Discover the owner's un-published partial log (activeLog_). Already
        // published logs live in globalFull_/list1_ and are handled by
        // DisposeAllPerPGCWork + DrainGlobalLFStackToMainQueue.
        WorkBuf* log = buffer.GetActiveLog();
        if (log != nullptr) {
            size_t n = log->len;
            size_t forwarded = 0;
            // Route-A: entries are already grey (shaded at write time), so enqueue
            // them for scanning directly — a re-discover would see them already
            // marked and wrongly skip scanning their fields.
            for (size_t i = 0; i < n; ++i) {
                BaseObject* obj = log->objs[i];
                if (obj == nullptr) {
                    continue;
                }
                if (UNLIKELY(!IsTraceableObjectAddress(obj) || !obj->IsValidObject())) {
                    continue;
                }
                mainQueue.PushMovedGrey(obj);
                forwarded++;
            }
            wbFlushStats_.Record(MSGCWriteBarrierFlushSource::FINAL_STW, n, forwarded);
            workBufPool_.Put(log);
            buffer.ActiveLogRef() = nullptr;
        }
        // Cycle end: return this owner's cached empty WorkBufs to the pool.
        buffer.DrainLogCacheToPool(workBufPool_);
    });
}

void MarkSweepCollector::DrainDeferredMainThreadQueueToMainQueue(GreyWorkQueue& mainQueue)
{
    std::lock_guard<std::mutex> lock(deferredMainThreadMutex_);
    while (!deferredMainThreadQueue_.empty()) {
        BaseObject* obj = deferredMainThreadQueue_.back();
        deferredMainThreadQueue_.pop_back();
        if (obj == nullptr) {
            continue;
        }
        if (UNLIKELY(!IsTraceableObjectAddress(obj))) {
            continue;
        }
        if (UNLIKELY(!obj->IsValidObject())) {
            continue;
        }
        mainQueue.PushMovedGrey(obj);
    }
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

    cycleStats_.markTermProcessedObjects += processed;
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
        VLOG(REPORT, "[MS-GC] CollectLargeGarbage: freed %zu large objects / %zu regions (%s B), cached %zu objects / %zu regions, %sus",
             cycleStats_.losFreedCount, cycleStats_.losFreedRegionCount,
             Pretty(cycleStats_.losFreedBytes).Str(),
             cycleStats_.losCachedCount, cycleStats_.losCachedRegionCount,
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
        cycleStats_.losCachedCount += stats.cachedObjectCount;
        cycleStats_.losCachedRegionCount += stats.cachedRegionCount;
    }
}

// ============================================================================
// VerifyMarkCompleteness: after marking, scan every marked object and report
// ref edges that point to an unmarked (soon-to-be-swept) target.
// ============================================================================
void MarkSweepCollector::VerifyMarkCompleteness()
{
    if (!IsVerifyMarkEnabled()) {
        return;
    }

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool == nullptr) {
        return;
    }

    constexpr size_t kMaxReport = 32;
    size_t violations = 0;
    size_t markedScanned = 0;
    size_t benignCurrentSweepGenTargets = 0;
    size_t staleManagedTargets = 0;
    size_t staleNonTraceableTargets = 0;
    size_t staleInvalidTargets = 0;
    size_t staleReported = 0;
    const uint32_t currentSweepGen = CentralRegionPool::GetGlobalSweepGen();

    centralPool->VisitMarkedObjects([&](BaseObject* obj) {
        markedScanned++;
        if (!IsTraceableObjectAddress(obj) || !obj->IsValidObject()) {
            return;
        }
        auto refFunc = [&](RefField<>& field) {
            BaseObject* target = field.GetTargetObject();
            if (target == nullptr ||
                !IsManagedAddress(reinterpret_cast<MAddress>(target))) {
                return;
            }

            const bool targetTraceable = IsTraceableObjectAddress(target);
            const bool targetValid = targetTraceable && target->IsValidObject();
            if (!targetTraceable || !targetValid) {
                staleManagedTargets++;
                if (!targetTraceable) {
                    staleNonTraceableTargets++;
                } else {
                    staleInvalidTargets++;
                }
                if (staleReported < kMaxReport &&
                    UNLIKELY(IsVerifyDetailEnabled() && IsInterestingDiagType(obj))) {
                    uintptr_t refState = LoadStateWordForDiag(obj);
                    uintptr_t tgtState = LoadStateWordForDiag(target);
                    const char* refType =
                        (obj->GetTypeInfo() != nullptr && obj->GetTypeInfo()->GetName() != nullptr)
                            ? obj->GetTypeInfo()->GetName() : "<?>";
                    VLOG(REPORT,
                         "[MS-GC][VERIFY] stale-ref: referrer=%p type=%s state=0x%zx -> "
                         "target=%p state=0x%zx traceable=%d valid=%d",
                         static_cast<void*>(obj),
                         refType,
                         refState,
                         static_cast<void*>(target),
                         tgtState,
                         static_cast<int>(targetTraceable),
                         static_cast<int>(targetValid));
                    std::string slotDesc = DescribeSlotForDiag(obj, field);
                    std::string refLayout = DescribeTypeLayoutForDiag(obj->GetTypeInfo());
                    VLOG(REPORT,
                         "[MS-GC][VERIFY] stale-ref-detail: referrer=%p %s layout={%s}",
                         static_cast<void*>(obj),
                         slotDesc.c_str(),
                         refLayout.c_str());
                    staleReported++;
                }
                return;
            }
            if (IsMarkedObject(target)) {
                return;
            }
            // Regions allocated/rebound after PreEnumRoots carry the current
            // sweep generation and are treated as "already swept" for this
            // cycle. Unmarked objects there are not reclaim candidates in the
            // current sweep, so do not count them as missed-mark violations.
            if (IsCurrentSweepGenObject(target, currentSweepGen)) {
                benignCurrentSweepGenTargets++;
                return;
            }

            // Marked referrer -> unmarked managed target in an older sweepGen:
            // target is a real reclaim candidate this cycle, so this is the
            // missed-mark UAF we want to catch.
            if (violations < kMaxReport) {
                uintptr_t refState = LoadStateWordForDiag(obj);
                uintptr_t tgtState = LoadStateWordForDiag(target);
                const char* refType =
                    (obj->GetTypeInfo() != nullptr && obj->GetTypeInfo()->GetName() != nullptr)
                        ? obj->GetTypeInfo()->GetName() : "<?>";
                const char* tgtType =
                    (target->GetTypeInfo() != nullptr && target->GetTypeInfo()->GetName() != nullptr)
                        ? target->GetTypeInfo()->GetName() : "<?>";
                VLOG(REPORT,
                     "[MS-GC][VERIFY] missed-mark: referrer=%p type=%s state=0x%zx -> "
                     "target=%p type=%s state=0x%zx traceable=%d valid=%d",
                     static_cast<void*>(obj),
                     refType,
                     refState,
                     static_cast<void*>(target),
                     tgtType,
                     tgtState,
                     static_cast<int>(targetTraceable),
                     static_cast<int>(targetValid));
                if (UNLIKELY(IsVerifyDetailEnabled() && IsInterestingDiagType(obj))) {
                    std::string slotDesc = DescribeSlotForDiag(obj, field);
                    std::string refLayout = DescribeTypeLayoutForDiag(obj->GetTypeInfo());
                    VLOG(REPORT,
                         "[MS-GC][VERIFY] missed-mark-detail: referrer=%p %s layout={%s}",
                         static_cast<void*>(obj),
                         slotDesc.c_str(),
                         refLayout.c_str());
                }
            }
            violations++;
        };
        ForEachRefFieldGCInline(obj, refFunc);
    });

    VLOG(REPORT,
         "[MS-GC][VERIFY] mark-completeness: scanned=%zu markedObjs, violations=%zu, "
         "benignCurrentSg=%zu, staleManaged=%zu, staleNonTraceable=%zu, staleInvalid=%zu",
         markedScanned, violations, benignCurrentSweepGenTargets,
         staleManagedTargets, staleNonTraceableTargets, staleInvalidTargets);
}

void MarkSweepCollector::ConcurrentSweep()
{
    uint64_t phaseStart = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- ConcurrentSweep ---");

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);

    // Compute object count and true live bytes in one stable bitmap walk.
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        MSMarkedStats markedStats = centralPool->ComputeMarkedStats();
        cycleStats_.markedObjects = markedStats.objectCount;
        cycleStats_.trueLiveBytes = markedStats.liveBytes;
        prevCycleMarkedObjects_.store(markedStats.objectCount, std::memory_order_relaxed);
        VLOG(REPORT, "[MS-GC] Mark bitmap: objects=%zu liveBytes=%zuB",
             markedStats.objectCount, markedStats.liveBytes);
    }

    BackgroundProcessor& backgroundProcessor = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
    backgroundProcessor.NotifyToSweep(kNoPreferredSweepClass);
    backgroundProcessor.NotifyToScavenge();

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
    if (obj->HasRefField()) {
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
    size_t largeCacheBytes = (centralPool != nullptr)
        ? centralPool->GetLargeHotCacheRegionCount() * MS_REGION_SIZE : 0;
    size_t largeCacheCount = largeCacheBytes / MS_REGION_SIZE;
    VLOG(REPORT, "[MS-GC] unswept: %zu regions, emptyCache: %zu regions, hotFree: %zu regions, largeCache: %zu regions (%zu KB)",
        unsweptCount, emptyCount, hotFreeCount, largeCacheCount, largeCacheBytes / 1024);
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
        CentralRegionPool* cpForAccounting = space.GetCentralPool();
        size_t emptyRegionCount = (cpForAccounting != nullptr)
            ? cpForAccounting->GetEmptySweptRegionCount() : 0;
        size_t hotFreeRegionCount = msRegionManager->GetHotFreeBytes() / MS_REGION_SIZE;
        size_t largeCacheRegionCount = (cpForAccounting != nullptr)
            ? cpForAccounting->GetLargeHotCacheRegionCount() : 0;
        VLOG(REPORT,
             "[MS-GC]   region-accounting: active=%zu regions, empty=%zu regions, hotFree=%zu regions, largeCache=%zu regions",
             cycleStats_.activeRegionCount,
             emptyRegionCount,
             hotFreeRegionCount,
             largeCacheRegionCount);

        // Per-shard snapshot: segments, total owned regions, used, free, hotFree (resident), coldFree (scavenged).
        uint32_t shardCount = msRegionManager->GetShardCount();
        VLOG(REPORT, "[MS-GC]   shard snapshots (P: segCnt / totalReg / used / free / hotFree / coldFree):");
        for (uint32_t sidx = 0; sidx < shardCount; ++sidx) {
            uint16_t sid = static_cast<uint16_t>(sidx);
            size_t segCnt = msRegionManager->GetOwnedSegmentCount(sid);
            size_t totalReg = msRegionManager->GetOwnedTotalRegionCount(sid);
            size_t freeReg = msRegionManager->GetOwnedFreeRegionCount(sid);
            size_t usedReg = (totalReg >= freeReg) ? (totalReg - freeReg) : 0;
            size_t hotFreeReg = msRegionManager->GetHotFreeRegionCount(sid);
            size_t coldFreeReg = msRegionManager->GetScavengedRegionCount(sid);
            VLOG(REPORT,
                 "[MS-GC]     shard[%u]: %zu seg, %zu reg, %zu used, %zu free (hot=%zu, cold=%zu)",
                 sidx, segCnt, totalReg, usedReg, freeReg, hotFreeReg, coldFreeReg);
        }
    }

    // CentralRegionPool 4-list breakdown
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        MSSweepCounts sweepCounts = centralPool->GetSweepCounts();
        VLOG(REPORT, "[MS-GC]   sweep-counts: bg=%zu regions, lazy=%zu regions, "
             "assist=%zu regions, helper=%zu regions",
             sweepCounts.backgroundSweptRegions,
             sweepCounts.lazySweptRegions,
             sweepCounts.assistSweptRegions,
             sweepCounts.helperSweptRegions);
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
    uint64_t updateStatsStart = TimeUtil::MicroSeconds();
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    GCStats& gcStats = GetGCStats();

    size_t oldThreshold = gcStats.GetThreshold();
    size_t oldScavengeThreshold = gcStats.GetScavengeThreshold();
    size_t oldLastTrueLiveBytes = gcStats.lastTrueLiveBytes;
    size_t preActiveRegionBytes = gcStats.lastRegionBytes;
    // Use mark-bit based live bytes (ComputeMarkedStats, computed in
    // PostMarkTermination). This counts only truly marked objects, so
    // allocations made during the current marking phase do not inflate the
    // next-cycle threshold.
    size_t liveBytes = cycleStats_.trueLiveBytes;
    size_t activeRegionBytes = space.GetHeapInUseBytes();

    gcStats.lastTrueLiveBytes = liveBytes;
    gcStats.lastRegionBytes = activeRegionBytes;
    gcStats.baselineAllocatedBytes = activeRegionBytes;

    // The raw MSGC target is trueLive * (1 + heapGrowth). Decreases follow the
    // raw target immediately; increases are rate-limited while preserving 25%
    // headroom above the current live set to avoid immediate retriggering.
    size_t baseBytes = liveBytes;

    double heapGrowth = CangjieRuntime::GetHeapParam().heapGrowth;
    double scavengeGrowth = CangjieRuntime::GetGCParam().scavengeGrowth;
    size_t gcThresholdCap = CangjieRuntime::GetGCParam().gcThreshold;
    // Floor to avoid GC thrash when base is tiny; cap at the configured gcThreshold.
    size_t minThreshold = std::min(gcThresholdCap, static_cast<size_t>(20) * 1024 * 1024);
    MSHeapThresholdDecision thresholdDecision =
        EvaluateMSHeapThreshold(oldThreshold, liveBytes, heapGrowth, minThreshold, gcThresholdCap);
    size_t newThreshold = thresholdDecision.threshold;
    size_t newScavengeThreshold = ComputeScavengeThresholdBytes(newThreshold, scavengeGrowth);
    gcStats.SetThreshold(newThreshold);
    gcStats.SetScavengeThreshold(newScavengeThreshold);
    gcStats.lastSurvivingRegionBytes = liveBytes;
    previousAssistProcessedObjects_ = cycleStats_.assistProcessedObjects;
    previousBackgroundProcessedObjects_ = cycleStats_.gcMainProcessedObjects +
                                          cycleStats_.helperProcessedObjects;
    VLOG(REPORT,
         "[MS-GC] heapThreshold: oldThreshold=%zuB newThreshold=%zuB "
         "oldScavengeThreshold=%zuB newScavengeThreshold=%zuB "
         "prevLive=%zuB trueLive=%zuB base=%zuB growth=%.2f scavengeGrowth=%.2f "
         "rawTarget=%zuB riseLimit=%zuB liveSafetyFloor=%zuB riseLimited=%d "
         "preActiveRegion=%zuB activeRegion=%zuB",
         oldThreshold, newThreshold, oldScavengeThreshold, newScavengeThreshold, oldLastTrueLiveBytes, liveBytes,
         baseBytes, 1.0 + heapGrowth, scavengeGrowth, thresholdDecision.rawTarget, thresholdDecision.riseLimit,
         thresholdDecision.liveSafetyFloor, static_cast<int>(thresholdDecision.riseLimited),
         preActiveRegionBytes, activeRegionBytes);

    VLOG(REPORT,
         "[MS-GC] live bytes %zu, activeRegion %zu, update gc threshold %zu -> %zu",
         liveBytes, activeRegionBytes, oldThreshold, gcStats.GetThreshold());
    uint64_t updateStatsUs = TimeUtil::MicroSeconds() - updateStatsStart;
    VLOG(REPORT, "[MS-GC] UpdateGCStats took %lu us", static_cast<unsigned long>(updateStatsUs));
    TRACE_COUNT("CJRT_post_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
