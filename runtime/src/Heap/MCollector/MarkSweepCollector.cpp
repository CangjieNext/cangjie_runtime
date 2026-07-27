// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MarkSweepCollector.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include "Base/CString.h"
#include "Base/EnvParam.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "CangjieRuntime.h"
#include "Common/GCRefFieldScan.inline.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSOOMEmergencyPolicy.h"
#include "Heap/MAllocator/MSParallelSweepPolicy.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSRegionManager.h"
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Heap/MAllocator/MSShardRouting.h"
#include "Heap/MBarrier/MSRefField.inline.h"
#include "Heap/MCollector/MSHeapThresholdPolicy.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Collector/ForwardDataManager.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"

#ifndef _WIN32
#include <unistd.h>  // usleep
#endif

namespace MapleRuntime {

namespace {
constexpr size_t kMSMarkEmptySpinRounds = 8;
constexpr uint64_t kMSMarkProgressWaitUs = 50;

bool IsMSGCReportEnabled()
{
#if defined(__OHOS__)
    return true;
#else
    return ENABLE_LOG(REPORT);
#endif
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

    if (IsLargeRegionEntryType(msInfo->regionType)) {
        msInfo = MSRegionInfoArray::ResolveLargeBaseInfo(msInfo);
        if (msInfo == nullptr) {
            return false;
        }
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

    if (IsLargeRegionEntryType(msInfo->regionType)) {
        MSRegionInfo* base = MSRegionInfoArray::ResolveLargeBaseInfo(msInfo);
        if (base == nullptr) {
            return false;
        }
        if (base->allocPtr == addr && (addr + sizeof(uintptr_t) <= base->regionEnd)) {
            outInfo = msInfo;
            return true;
        }
        return false;
    }

    if (msInfo->regionType == MSRegionType::MS_REGION_FREE ||
        msInfo->regionType == MSRegionType::MS_REGION_EMPTY_CACHE) {
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

// A validated object-to-mark mapping. Small objects retain the resolved bitmap
// coordinate; large objects retain the canonical base entry. This lets
// DiscoverObject probe and update the mark state without resolving the Region
// or recomputing the slot index a second time.
struct ResolvedMarkTarget {
    MSRegionInfo* info = nullptr;
    size_t wordIdx = 0;
    uint64_t bitMask = 0;
    bool isLarge = false;

    bool IsMarked() const
    {
        return isLarge ? info->IsMarked() : info->IsMarkBitSetResolved(wordIdx, bitMask);
    }

    bool TryMark() const
    {
        if (isLarge) {
            return !info->AtomicTestAndSetMarked();
        }
        return info->MarkBitmapWordResolved(wordIdx, bitMask) != 0;
    }
};

static inline bool ResolveMarkTargetImpl(BaseObject* obj, ResolvedMarkTarget& out,
                                         bool knownManaged)
{
    out = ResolvedMarkTarget {};
    if (obj == nullptr) {
        return false;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (!knownManaged && !IsManagedAddress(addr)) {
        return false;
    }

    size_t regionOffset = 0;
    MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoDirect(addr, regionOffset);

    if (IsLargeRegionEntryType(info->regionType)) {
        MSRegionInfo* base = MSRegionInfoArray::ResolveLargeBaseInfo(info);
        if (base == nullptr ||
            base->allocPtr != addr || base->regionEnd < addr ||
            base->regionEnd - addr < sizeof(uintptr_t)) {
            return false;
        }
        out.info = base;
        out.isLarge = true;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        CHECK_DETAIL(MSRegionInfoArray::GetRegionInfoValidated(addr) == info,
                     "[MS-GC] direct LARGE Region lookup disagrees with validated lookup: "
                     "addr=%p direct=%p",
                     reinterpret_cast<void*>(addr),
                     static_cast<void*>(info));
#endif
        return true;
    }

    if (info->regionType == MSRegionType::MS_REGION_FREE ||
        info->regionType == MSRegionType::MS_REGION_EMPTY_CACHE ||
        info->sizeClass >= kNumSizeClasses || info->markBitmap == nullptr ||
        info->totalSlotCount == 0) {
        return false;
    }

    size_t slotSize = info->GetSlotSize();
    if (slotSize == 0) {
        return false;
    }

    size_t slotIdx = SizeClassInfo::OffsetToSlotIndex(info->sizeClass, regionOffset);
    if (slotIdx >= info->totalSlotCount || slotIdx * slotSize != regionOffset) {
        return false;
    }
    if (info->regionType == MSRegionType::MS_REGION_BUMP && addr >= info->allocPtr) {
        return false;
    }

    out.info = info;
    out.wordIdx = slotIdx / 64;
    out.bitMask = 1ULL << (slotIdx % 64);
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    CHECK_DETAIL(MSRegionInfoArray::GetRegionInfoValidated(addr) == info,
                 "[MS-GC] direct small Region lookup disagrees with validated lookup: "
                 "addr=%p direct=%p regionOffset=%zu",
                 reinterpret_cast<void*>(addr),
                 static_cast<void*>(info),
                 regionOffset);
#endif
    return true;
}

static inline bool ResolveMarkTarget(BaseObject* obj, ResolvedMarkTarget& out)
{
    return ResolveMarkTargetImpl(obj, out, false);
}

static inline bool ResolveManagedMarkTarget(BaseObject* obj, ResolvedMarkTarget& out)
{
    return ResolveMarkTargetImpl(obj, out, true);
}

// Static member definition
std::atomic<MarkSweepCollector*> MarkSweepCollector::activeCollector_{nullptr};

// ============================================================================
// Lifecycle
// ============================================================================

void MarkSweepCollector::Init()
{
    LOG(RTLOG_REPORT, "[MS-GC] MarkSweepCollector::Init()");
    MSWorkBufScaleConfig workBufConfig = CangjieRuntime::GetMSWorkBufScaleConfig();
    CHECK_DETAIL(workBufPool_.Configure(workBufConfig.capacity),
                 "MSGC WorkBufPool cannot configure capacity=%zu after allocation",
                 workBufConfig.capacity);
    LOG(RTLOG_REPORT,
        "[MS-GC] WorkBuf scale: P=%u capacity=%zu checkpointObjects=%zu "
        "backlogObjectsPerLevel=%zu helperPromoteObjects=%zu",
        workBufConfig.processors,
        workBufConfig.capacity,
        workBufConfig.checkpointObjects,
        workBufConfig.backlogObjectsPerLevel,
        workBufConfig.helperPromoteObjects);
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
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    cycleStats_.markStartInUseBytes = space.GetHeapInUseBytes();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    DumpBeforeGC();
#endif
    TRACE_COUNT("CJRT_pre_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

void MarkSweepCollector::PostGarbageCollection(uint64_t gcIndex)
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    // MarkTermination has already restored the idle barrier before Mutators
    // resume.  The Sweep epoch is tracked independently by CentralRegionPool.
    collectorResources.NotifyGCFinished(gcIndex);

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    DumpAfterGC();
#endif
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

    PreGarbageCollection(/* isConcurrent */ true);
    DumpGCStateSnapshot("preGCMark");
    DoGarbageCollection();
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

void MarkSweepCollector::DoGarbageCollection()
{
    PreEnumRoots();        // Step 2.1a
    EnumRoots();            // Step 2.1b
    ConcurrentMark();      // Step 2.2
    MarkTermination();     // Step 2.3
    PostMarkTermination(); // Step 2.4
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

bool MarkSweepCollector::DiscoverObject(BaseObject* obj, TypeInfo** markedTypeInfo)
{
    if (markedTypeInfo != nullptr) {
        *markedTypeInfo = nullptr;
    }
    ResolvedMarkTarget target;
    if (UNLIKELY(!ResolveMarkTarget(obj, target))) {
        return false;
    }
    if (target.IsMarked()) {
        return false;
    }
    // An unmarked, aligned slot may still be stale and zeroed after Sweep.
    // Acquire and retain its TypeInfo before this call is allowed to set the
    // mark bit, so the winner does not reload stateWord for HasRef filtering.
    TypeInfo* typeInfo = obj->GetTypeInfo();
    if (UNLIKELY(typeInfo == nullptr)) {
        return false;
    }
    if (!target.TryMark()) {
        return false;
    }
    if (markedTypeInfo != nullptr) {
        *markedTypeInfo = typeInfo;
    }
    return true;
}

bool MarkSweepCollector::DiscoverManagedObject(BaseObject* obj, TypeInfo** markedTypeInfo)
{
    if (markedTypeInfo != nullptr) {
        *markedTypeInfo = nullptr;
    }
    ResolvedMarkTarget target;
    if (UNLIKELY(!ResolveManagedMarkTarget(obj, target))) {
        return false;
    }
    if (target.IsMarked()) {
        return false;
    }
    TypeInfo* typeInfo = obj->GetTypeInfo();
    if (UNLIKELY(typeInfo == nullptr)) {
        return false;
    }
    if (!target.TryMark()) {
        return false;
    }
    if (markedTypeInfo != nullptr) {
        *markedTypeInfo = typeInfo;
    }
    return true;
}

bool MarkSweepCollector::ResolveManagedSmallRefForBarrier(BaseObject* ref, MSRegionInfo*& info,
                                                          size_t& wordIdx, uint64_t& bitMask,
                                                          TypeInfo*& whiteTypeInfo) const
{
    info = nullptr;
    whiteTypeInfo = nullptr;
    ResolvedMarkTarget target;
    if (UNLIKELY(!ResolveManagedMarkTarget(ref, target)) || target.isLarge) {
        return false;
    }
    // Already-marked entries are safe to batch without touching stateWord:
    // MarkBitmapWord will return no winning bit, so Flush never dereferences
    // the candidate. White entries still require stale-slot validation.
    if (!target.IsMarked()) {
        whiteTypeInfo = ref->GetTypeInfo();
        if (UNLIKELY(whiteTypeInfo == nullptr)) {
            return false;
        }
    }
    info = target.info;
    wordIdx = target.wordIdx;
    bitMask = target.bitMask;
    return true;
}

bool MarkSweepCollector::TryEnqueueGreyObject(BaseObject* obj, GreyWorkSink& sink)
{
    TypeInfo* typeInfo = nullptr;
    bool newlyMarked = DiscoverObject(obj, &typeInfo);
    if (newlyMarked && typeInfo->HasRefField()) {
        sink.PushGrey(obj);
    }
    return newlyMarked;
}

bool MarkSweepCollector::TryEnqueueGreyObject(BaseObject* obj, GreyWorkQueue& queue)
{
    TypeInfo* typeInfo = nullptr;
    bool newlyMarked = DiscoverObject(obj, &typeInfo);
    if (newlyMarked && typeInfo->HasRefField()) {
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

    // 9. Reset cycle-global assist state. Per-P feedback is reset while
    // initializing each AllocBufferMS below.
    {
        std::lock_guard<std::mutex> lock(exitedAssistStatsMutex_);
        exitedAssistStats_ = MSMarkAssistStats{};
    }
    bgScanCredit_.store(0, std::memory_order_relaxed);

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
        bool disableAssist = ReadBoolParameter("CJ_MS_DISABLE_ASSIST", false);
        uint64_t initialRatioScaled = ApplyMSAssistDisableOverride(decision.nextRatio, disableAssist);
        if (!disableAssist) {
            previousAssistBaseRatioScaled_ = initialRatioScaled;
            if (prevMarked != 0 && allocSize != 0) {
                assistBaseRatioInitialized_ = true;
            }
        }
        assistWorkPerByteScaled_.store(initialRatioScaled, std::memory_order_release);
        VLOG(REPORT,
             "[MS-GC] Assist pacing: disabled=%d effectiveRatio=%llu basePercent=%llu "
             "(prevMarked=%zu, allocSize=%zu)",
             static_cast<int>(disableAssist),
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

    // 11. Stop assist by clearing ratio. WeakRefs deferred by concurrent
    // workers remain in deferredMainThreadQueue_ and are classified only after
    // all mutators have stopped in MarkTermination.
    assistWorkPerByteScaled_.store(0, std::memory_order_release);
}

void MarkSweepCollector::RunConcurrentMarkLoop()
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- ConcurrentMark (gcThreadWork_ + globalFull_ LFStack) ---");

    // Init GC thread's own PerPGCWork (sets its global queue and pool).
    // Then pack all root objects into WorkBufs and push directly to globalFull_.
    // GC thread and assist threads both compete for work from globalFull_.
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
    bool isOOM = gcReason == GC_REASON_OOM;
    LFStack::Counts initialCounts = globalFull_.GetApproxCounts();
    size_t initialDebt = initialCounts.objects;
    const size_t maxAssistMultiplier = CangjieRuntime::GetGCParam().msAssistMaxMultiplier;
    MSMarkPressurePolicy pressurePolicy(maxHelpers, maxAssistMultiplier, isOOM, initialDebt);
    cycleStats_.markDebtBuffers = initialCounts.total;
    cycleStats_.markDebtObjects = initialDebt;
    cycleStats_.markPressureLevel = pressurePolicy.GetPressureLevel();
    size_t activeHelpers = pressurePolicy.GetTargetHelpers();
    StartMarkWorkers(maxHelpers, activeHelpers);

    // Preserve the legacy configuration in object units. The old value counted
    // 1024-object WorkBuf rounds; using scanned objects keeps the decay window
    // stable when the runtime-selected WorkBuf capacity changes.
    const size_t creditDecayRounds = CangjieRuntime::GetGCParam().msAssistCreditDecayRounds;
    const size_t creditDecayObjects =
        creditDecayRounds > std::numeric_limits<size_t>::max() /
                                kMSAssistCreditDecayReferenceCapacity
            ? std::numeric_limits<size_t>::max()
            : creditDecayRounds * kMSAssistCreditDecayReferenceCapacity;
    size_t creditDecayProcessedObjects = 0;

    // Helper width and assist pacing share one monotonic pressure level.
    const uint64_t baseAssistRatio = assistWorkPerByteScaled_.load(std::memory_order_acquire);
    uint64_t publishedAssistRatio = ScaleMSAssistRatioForPressure(
        baseAssistRatio, pressurePolicy.GetAssistMultiplier());
    if (publishedAssistRatio != baseAssistRatio) {
        assistWorkPerByteScaled_.store(publishedAssistRatio, std::memory_order_release);
    }
    VLOG(REPORT,
         "[MS-GC] [mark-pressure-start] reason=%s debtObjects=%zu debtBuffers=%zu level=%zu "
         "helpers=%zu assistMultiplier=%zu forceAll=%d",
         g_gcRequests[gcReason].name,
         initialDebt,
         initialCounts.total,
         pressurePolicy.GetPressureLevel(),
         activeHelpers,
         pressurePolicy.GetAssistMultiplier(),
         static_cast<int>(isOOM));

    uint64_t quiesceDurationUs = 0;
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
    size_t completedMarkBuffers = 0;
    size_t processedSinceCheckpoint = 0;
    size_t emptyPollRounds = 0;

    while (true) {
        // Capture before checking the work predicates. A publication or final
        // in-flight exit racing with the checks changes the epoch and makes the
        // later wait return immediately instead of losing the notification.
        uint64_t observedProgressEpoch = markWorkEpoch_.Load();

        // Every published buffer is one GC-main mark slice, regardless of length.
        LFStack::Counts backlog { 0, 0, 0 };
        bool processedBuffer = false;
        size_t scannedCount = DrainOneBufRound(
            gcThreadWork_, false, &backlog, &processedBuffer);
        gcMainProcessed += scannedCount;
        processedSinceCheckpoint += scannedCount;
        if (processedBuffer) {
            ++completedMarkBuffers;
        }
        // The Pop snapshot precedes scanning. A non-empty scan can publish new
        // output, so refresh from the lock-free scheduling snapshot before
        // evaluating convergence or the next pressure checkpoint. A failed Pop already
        // returned an exact empty-time snapshot under the queue mutex.
        if (processedBuffer) {
            backlog = globalFull_.GetApproxCounts();
        }

        size_t publishedBacklogBuffers = backlog.total;
        size_t publishedBacklogObjects = backlog.objects;

        // Helper work is intentionally excluded from assist credit. Helpers keep
        // only exit-time diagnostics; GC-main work remains the sole background
        // credit source.
        size_t backgroundScanned = scannedCount;
        if (creditDecayProcessedObjects < creditDecayObjects && backgroundScanned > 0) {
            double w = static_cast<double>(
                           creditDecayObjects - creditDecayProcessedObjects) /
                       static_cast<double>(creditDecayObjects);
            size_t inj = static_cast<size_t>(static_cast<double>(backgroundScanned) * w);
            if (inj > 0) {
                bgScanCredit_.fetch_add(static_cast<int64_t>(inj), std::memory_order_release);
            }
            creditDecayProcessedObjects =
                backgroundScanned > creditDecayObjects - creditDecayProcessedObjects
                    ? creditDecayObjects
                    : creditDecayProcessedObjects + backgroundScanned;
        }

        MSMarkWorkerControl::Snapshot controlSnapshot = markWorkerControl_.GetSnapshot();
        bool gcMainInputEmpty = gcThreadWork_.ConcurrentInputEmpty();
        bool convergenceCandidate = gcMainInputEmpty && publishedBacklogBuffers == 0 &&
                                    controlSnapshot.workersInFlight == 0 &&
                                    controlSnapshot.assistsInFlight == 0;
        if (convergenceCandidate) {
            // Approximate counts avoid a second queue lock on ordinary rounds.
            // Zero is confirmed under the queue mutex before one-way quiesce.
            if (globalFull_.GetCounts().total != 0) {
                emptyPollRounds = 0;
                continue;
            }
            cycleStats_.markTerminationReason = MSMarkTerminationReason::NATURAL;
            uint64_t quiesceStartUs = TimeUtil::MicroSeconds();
            quiesceResidualBuffers = QuiesceMarkWorkers();
            quiesceDurationUs = TimeUtil::MicroSeconds() - quiesceStartUs;
            break;
        }

        bool shouldForce = false;
        MSMarkTerminationReason terminationReason = MSMarkTerminationReason::NATURAL;
        if (processedSinceCheckpoint >= MSMarkPressurePolicy::kCheckpointObjects) {
            processedSinceCheckpoint = 0;
            MSMarkCheckpointDecision decision = pressurePolicy.Checkpoint(publishedBacklogObjects);
            ++cycleStats_.markCheckpointCount;
            cycleStats_.markDebtBuffers = publishedBacklogBuffers;
            cycleStats_.markDebtObjects = decision.currentDebt;
            cycleStats_.markPressureLevel = decision.pressureLevel;
            cycleStats_.markNoProgressWindows = decision.noProgressWindows;

            if (decision.targetHelpers > activeHelpers) {
                activeHelpers = decision.targetHelpers;
                markWorkerControl_.SetTargetHelpers(activeHelpers);
            }
            uint64_t adjustedRatio = ScaleMSAssistRatioForPressure(
                baseAssistRatio, decision.assistMultiplier);
            if (adjustedRatio != publishedAssistRatio) {
                assistWorkPerByteScaled_.store(adjustedRatio, std::memory_order_release);
                publishedAssistRatio = adjustedRatio;
            }

            int64_t debtDelta = static_cast<int64_t>(decision.previousDebt) -
                                static_cast<int64_t>(decision.currentDebt);
            VLOG(REPORT,
                 "[MS-GC] [mark-pressure] completedBuffers=%zu debtObjects=%zu "
                 "debtBuffers=%zu previousDebtObjects=%zu "
                 "debtDelta=%lld noProgress=%zu level=%zu helpers=%zu "
                 "assistMultiplier=%zu",
                 completedMarkBuffers,
                 decision.currentDebt,
                 publishedBacklogBuffers,
                 decision.previousDebt,
                 static_cast<long long>(debtDelta),
                 decision.noProgressWindows,
                 decision.pressureLevel,
                 decision.targetHelpers,
                 decision.assistMultiplier);

            if (decision.ShouldTerminate()) {
                shouldForce = true;
                terminationReason = MSMarkTerminationReason::DEBT_STALLED;
            }
        }

        // Keep the configured fixed time budget as an independent fallback.
        bool configuredBudgetExpired =
            markBudgetUs != 0 && TimeUtil::MicroSeconds() - t0 >= markBudgetUs;
        if (!shouldForce && configuredBudgetExpired) {
            shouldForce = true;
            terminationReason = MSMarkTerminationReason::TIME_BUDGET;
        }

        if (shouldForce) {
            forceConverged = true;
            cycleStats_.markTerminationReason = terminationReason;
            cycleStats_.markDebtBuffers = publishedBacklogBuffers;
            cycleStats_.markDebtObjects = publishedBacklogObjects;
            cycleStats_.markPressureLevel = pressurePolicy.GetPressureLevel();
            VLOG(REPORT,
                 "[MS-GC] [mark-termination-request] reason=%s debtObjects=%zu "
                 "debtBuffers=%zu "
                 "level=%zu noProgress=%zu",
                 GetMSMarkTerminationReasonName(terminationReason),
                 publishedBacklogObjects,
                 publishedBacklogBuffers,
                 pressurePolicy.GetPressureLevel(),
                 cycleStats_.markNoProgressWindows);
            uint64_t quiesceStartUs = TimeUtil::MicroSeconds();
            quiesceResidualBuffers = QuiesceMarkWorkers();
            quiesceDurationUs = TimeUtil::MicroSeconds() - quiesceStartUs;
            break;
        }

        size_t inFlight = controlSnapshot.workersInFlight + controlSnapshot.assistsInFlight;
        if (gcMainInputEmpty && publishedBacklogBuffers == 0 && inFlight != 0) {
            if (emptyPollRounds < kMSMarkEmptySpinRounds) {
                ++emptyPollRounds;
                std::this_thread::yield();
            } else {
                (void)markWorkEpoch_.WaitForChange(
                    observedProgressEpoch, std::chrono::microseconds(kMSMarkProgressWaitUs));
                emptyPollRounds = 0;
            }
        } else {
            emptyPollRounds = 0;
        }
    }

    cycleStats_.concurrentMarkUs = TimeUtil::MicroSeconds() - t0;
    cycleStats_.gcMainProcessedBuffers = completedMarkBuffers;
    VLOG(REPORT,
         "[MS-GC] ConcurrentMark time: %sus (forceConverged=%d reason=%s "
         "completedBuffers=%zu debtObjects=%zu debtBuffers=%zu "
         "level=%zu noProgress=%zu)",
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         static_cast<int>(forceConverged),
         GetMSMarkTerminationReasonName(cycleStats_.markTerminationReason),
         completedMarkBuffers,
         cycleStats_.markDebtObjects,
         cycleStats_.markDebtBuffers,
         cycleStats_.markPressureLevel,
         cycleStats_.markNoProgressWindows);
    size_t helperProcessed = 0;
    for (const auto& slot : markWorkerSlots_) {
        helperProcessed += slot->stats.TotalProcessed();
    }
    cycleStats_.gcMainProcessedObjects = gcMainProcessed;
    cycleStats_.helperProcessedObjects = helperProcessed;
    VLOG(REPORT,
         "[MS-GC] [mark-workers] configured=%zu processed=%zu workersInFlight=%zu "
         "assistInFlight=%zu mainProcessed=%zu mainBuffers=%zu",
         maxHelpers + 1,
         helperProcessed,
         markWorkerControl_.GetWorkersInFlight(),
         markWorkerControl_.GetAssistsInFlight(),
         gcMainProcessed,
         completedMarkBuffers);
    VLOG(REPORT,
         "[MS-GC] [mark-quiesce] duration=%sus quiesceResidualBuffers=%zu",
         Pretty(quiesceDurationUs).Str(),
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
    // Sweep uses the same physical pool and may leave a smaller active width.
    // MARK owns the pool lease here. Restore the full potential width before
    // submitting long-lived tasks; targetHelpers gates actual scan activity.
    threadPool->SetMaxActiveThreadNum(static_cast<int32_t>(maxHelpers));
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

        uint64_t observedProgressEpoch = markWorkEpoch_.Load();
        while (markWorkerControl_.CanContinueWorker(workerId)) {
            observedProgressEpoch = markWorkEpoch_.Load();
            bool acquired = slot.work.PromotePutToGet(
                                std::max<size_t>(1, workBufPool_.Capacity() / 8)) ||
                            slot.work.AcquireOneBuffer() ||
                            slot.work.PromotePutToGet(1);
            if (!acquired) {
                break;
            }

            size_t scanned = DrainCurrentBuf(slot.work);
            if (scanned != 0) {
                slot.stats.RecordScanned(scanned);
            }
        }

        markWorkerControl_.ExitWorker();
        if (markWorkerControl_.CanContinueWorker(workerId)) {
            (void)markWorkEpoch_.WaitForChange(
                observedProgressEpoch, std::chrono::microseconds(kMSMarkProgressWaitUs));
        }
    }
    slot.work.Dispose();
}

size_t MarkSweepCollector::QuiesceMarkWorkers()
{
    markWorkerControl_.BeginQuiesce();
    markWorkerControl_.WaitForNoInFlight();
    // Do not publish private partial putCur_ tails here. Under sustained write-
    // barrier traffic, repeatedly closing those tails concurrently can prevent
    // convergence while every new allocation is conservatively marked live.
    // MarkTermination disposes and drains the bounded tails under STW.
    size_t residualBuffers = globalFull_.GetApproxCounts().total;
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
// TraceObjectRefFieldsToGCWork: trace all ref fields of obj, push grey children
// into PerPGCWork (Step5 replacement for TraceObjectRefFields on WorkStack).
// ============================================================================
void MarkSweepCollector::TraceObjectRefFieldsToGCWork(BaseObject* obj, TypeInfo* typeInfo,
                                                      PerPGCWork& gcw)
{
    auto refFunc = [this, &gcw](RefField<>& field) {
        BaseObject* target = MSLoadRawReference(field);
        TypeInfo* targetTypeInfo = nullptr;
        bool newlyMarked = DiscoverObject(target, &targetTypeInfo);
        if (newlyMarked && targetTypeInfo->HasRefField()) {
            gcw.PutObj(target);
        }
    };
    ForEachRefFieldGCKnownType(obj, typeInfo, refFunc);
}

// ============================================================================
// TransferRootsToGCWork: pack root objects into WorkBufs and push directly to
// globalFull_, same as activeLog_ overflow does.
// GC thread and assist threads both consume from globalFull_.
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

void MarkSweepCollector::TraceRefField(RefField<>& field, WorkStack& workStack)
{
    BaseObject* target = MSLoadRawReference(field);
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

// The write-barrier log is a WorkBuf pointer (activeLog_) that holds old refs
// which already won the mark CAS. Overflow publishes the buffer wholesale and
// installs a fresh one.
WorkBuf* MarkSweepCollector::EnsureActiveLog(AllocBufferMS& ab)
{
    WorkBuf* log = ab.GetActiveLog();
    if (log == nullptr) {
        log = workBufPool_.Get();
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
    // Entries were already shaded by the barrier, so consumers scan directly.
    // Make every complete log visible to the GC thread and all assists.
    globalFull_.Push(full);

    ab.ActiveLogRef() = workBufPool_.Get();
}

void MarkSweepCollector::PublishExitedLog(AllocBufferMS& ab)
{
    MergeExitedAssistStats(ab);
    WorkBuf* log = ab.GetActiveLog();
    if (log != nullptr) {
        if (!log->Empty()) {
            // Owner is leaving: publish directly; entries are already grey.
            globalFull_.Push(log);
        } else {
            workBufPool_.Put(log);
        }
        ab.ActiveLogRef() = nullptr;
    }
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

    cycleStats_.assistProcessedObjects = total.processed;
    VLOG(REPORT, "[MS-GC] [assist] processed=%zu", total.processed);
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

void MarkSweepCollector::PushTrustedGreyToDeferredMainThread(BaseObject* obj)
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (UNLIKELY(obj == nullptr || !IsTraceableObjectAddress(obj) ||
                 !obj->IsValidObject() || !IsMarkedObject(obj) ||
                 !obj->HasRefField())) {
        LOG(RTLOG_FATAL, "[MS-GC] invalid trusted-grey deferred object: %p",
            static_cast<void*>(obj));
    }
#endif
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
            BaseObject* obj = buf->Objects()[i];
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
            if (obj == nullptr) {
                continue;
            }
            if (UNLIKELY(!IsTraceableObjectAddress(obj))) {
                continue;
            }
            if (UNLIKELY(!obj->IsValidObject())) {
                continue;
            }
#endif
            mainQueue.PushMovedGrey(obj);
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
size_t MarkSweepCollector::DrainOneBufRound(PerPGCWork& gcw, bool requireFull,
                                           LFStack::Counts* countsAfterPop,
                                           bool* processedBuffer)
{
    if (processedBuffer != nullptr) {
        *processedBuffer = false;
    }
    bool acquired = false;
    if (requireFull) {
        acquired = gcw.AcquireOneFullBuffer();
        if (countsAfterPop != nullptr) {
            *countsAfterPop = globalFull_.GetApproxCounts();
        }
    } else if (countsAfterPop != nullptr) {
        acquired = gcw.AcquireOneBuffer(*countsAfterPop);
    } else {
        acquired = gcw.AcquireOneBuffer();
    }
    if (!acquired) {
        return 0;
    }
    if (processedBuffer != nullptr) {
        *processedBuffer = true;
    }
    return DrainCurrentBuf(gcw);
}

size_t MarkSweepCollector::DrainCurrentBuf(PerPGCWork& gcw)
{
    WorkBuf* input = gcw.getCur_;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    size_t nProcessed = 0;
#else
    size_t nProcessed = input == nullptr ? 0 : input->len;
#endif
    while (input != nullptr && !input->Empty()) {
        BaseObject* obj = input->Pop();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        if (UNLIKELY(!IsTraceableObjectAddress(obj))) {
            LOG(RTLOG_FATAL, "[MS-GC] DrainCurrentBuf: FATAL non-traceable "
                "trusted-grey obj=%p", static_cast<void*>(obj));
        }
#endif
        TypeInfo* typeInfo = obj->GetTypeInfo();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        if (UNLIKELY(typeInfo == nullptr || !IsMarkedObject(obj) ||
                     !typeInfo->HasRefField())) {
            LOG(RTLOG_FATAL, "[MS-GC] DrainCurrentBuf: FATAL invalid trusted-grey obj=%p, "
                "stateWord=0x%zx, markBit=%d, hasRef=%d",
                (void*)obj,
                *reinterpret_cast<uintptr_t*>(obj),
                (int)IsMarkedObject(obj),
                typeInfo == nullptr ? 0 : (int)typeInfo->HasRefField());
        }
        nProcessed++;
#endif
        if (UNLIKELY(typeInfo->IsWeakRefType())) {
            PushTrustedGreyToDeferredMainThread(obj);
            continue;
        }
        TraceObjectRefFieldsToGCWork(obj, typeInfo, gcw);
    }
    // Never cascade into partial putCur_ in the same slice.
    if (input != nullptr) {
        gcw.RecycleCurrentInput();
    }
    return nProcessed;
}

// ============================================================================
// MSMarkAssist: consume at most one full global input per call. After claiming
// that input, merge newly discovered objects into one Global Partial builder and
// publish the builder before returning.
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
    PerPGCWork& gcw = allocBuffer.GetGCWork();
    if (!gcw.IsInitialized()) {
        gcw.Init(&globalFull_, &workBufPool_);
    }

    // ① Steal GC-supplied credit.
    size_t paidByBg = TryStealBgCreditPartial(debtObjects);

    // ② One full global buffer pays exactly one buffer of remaining debt.
    size_t remainDebt = (debtObjects > paidByBg) ? (debtObjects - paidByBg) : 0;
    size_t paidByGlobal = 0;
    size_t workBufCapacity = workBufPool_.Capacity();
    if (remainDebt >= workBufCapacity && gcw.AcquireOneFullBuffer()) {
        WorkBuf* output = gcw.AcquirePartialOutputBuffer();
        CHECK_DETAIL(output != nullptr, "MSGC Mark Assist failed to acquire an output WorkBuf");
        size_t nDrained = DrainCurrentBuf(gcw);
        gcw.PublishPartialOutput();
        if (nDrained > 0) {
            paidByGlobal = workBufCapacity;
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
    VLOG(REPORT,
         "[MS-GC] [mark-stw-entry] branch=%s gcReason=%s debtObjects=%zu "
         "debtBuffers=%zu level=%zu "
         "noProgress=%zu",
         GetMSMarkTerminationReasonName(cycleStats_.markTerminationReason),
         g_gcRequests[gcReason].name,
         cycleStats_.markDebtObjects,
         cycleStats_.markDebtBuffers,
         cycleStats_.markPressureLevel,
         cycleStats_.markNoProgressWindows);

    // STW: freeze all mutators
    ScopedStopTheWorld stw("MS MarkTermination", true, GCPhase::GC_PHASE_MS_MARK_TERMINATION);

    // Now that all mutators are frozen, clear activeCollector_.
    activeCollector_.store(nullptr, std::memory_order_release);

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    SnapshotAssistStats();

    GreyWorkQueue mainQueue;
    // 1. Complete the ordinary mark closure first. Finalizer reachability must
    // be classified against this stable bitmap, before resurrection marks are
    // added to it.
    size_t pendingSourceEntries = DrainMarkTerminationToFixedPoint(mainQueue);

    // 2. Snapshot the registered Finalizer population and move only objects
    // that are unreachable in the ordinary closure into the finalizable roots.
    PrepareFinalizersForResurrection();

    // 3. Mark newly-finalizable objects and their strong children to complete
    // the resurrection closure. Weak referents encountered here are registered
    // but are not followed as strong edges.
    CollectResurrectionCandidatesToMain(mainQueue);
    size_t resurrectionEntries = mainQueue.size();
    pendingSourceEntries += DrainMarkTerminationToFixedPoint(mainQueue);

    // 4. The final mark bitmap is now stable. Resolve every live WeakRef before
    // mutators resume and before any Region can be published for sweep.
    ClearWeakRefsAfterMark();

    // 5. Diagnostic: marking is now complete and all mutators are frozen
    // (STW). This is the authoritative window to verify mark completeness —
    // no concurrent mutation can produce false positives. No-op unless the
    // env var cjMSVerifyMark is set.
    VerifyMarkCompleteness();

    // 6. Reset all per-P gcWork and return WorkBufs to pool.
    // NOTE: sweep generation is flipped in PreEnumRoots (before mark), NOT
    // here. Flipping after mark would let regions allocated during this
    // cycle's mark phase keep the old generation and be swept this cycle,
    // wrongly reclaiming live stack-only objects (not covered by the barrier).
    ResetAllPerPGCWork();

    cycleStats_.markTerminationUs = TimeUtil::MicroSeconds() - phaseStart;
    VLOG(REPORT,
         "[MS-GC] MarkTermination total: %sus, flushed=%zu entries, resurrection=%zu entries",
         Pretty(cycleStats_.markTerminationUs).Str(),
         pendingSourceEntries,
         resurrectionEntries);

    // Restore IDLE INSIDE STW so that when Mutators resume, the mark barrier
    // has already been disabled. This eliminates a gap where Mutators could
    // still buffer write-barrier entries after the last drain. The independent
    // Sweep epoch remains open and is tracked by CentralRegionPool counters.
    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, false);

    MSPostMarkSweepPlan sweepPlan = BuildMSPostMarkSweepPlan(gcReason == GC_REASON_OOM);
    if (centralPool != nullptr && sweepPlan.reenableAtMarkTermination) {
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
        // Move the owner's unpublished partial log (activeLog_). Already
        // published logs live in globalFull_ and are handled by
        // DisposeAllPerPGCWork + DrainGlobalLFStackToMainQueue.
        WorkBuf* log = buffer.GetActiveLog();
        if (log != nullptr) {
            size_t n = log->len;
            // Route-A: entries are already grey (shaded at write time), so enqueue
            // them for scanning directly — a re-discover would see them already
            // marked and wrongly skip scanning their fields.
            for (size_t i = 0; i < n; ++i) {
                BaseObject* obj = log->Objects()[i];
                if (obj == nullptr) {
                    continue;
                }
                if (UNLIKELY(!IsTraceableObjectAddress(obj) || !obj->IsValidObject())) {
                    continue;
                }
                mainQueue.PushMovedGrey(obj);
            }
            workBufPool_.Put(log);
            buffer.ActiveLogRef() = nullptr;
        }
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

size_t MarkSweepCollector::DrainMarkTerminationToFixedPoint(GreyWorkQueue& mainQueue)
{
    size_t sourceEntries = 0;
    size_t drainRounds = 0;
    do {
        drainRounds++;
        ExecuteLikeDrainMainQueue(mainQueue);

        size_t before = mainQueue.size();
        FlushAllWriteBarrierBuffers(mainQueue);
        DisposeAllPerPGCWork();
        DrainGlobalLFStackToMainQueue(mainQueue);
        DrainDeferredMainThreadQueueToMainQueue(mainQueue);
        sourceEntries += mainQueue.size() - before;
    } while (!mainQueue.empty());

    if (sourceEntries != 0 || drainRounds > 1) {
        VLOG(REPORT,
             "[MS-GC] MarkTermination fixed-point drain: rounds=%zu sourceEntries=%zu",
             drainRounds,
             sourceEntries);
    }
    return sourceEntries;
}

void MarkSweepCollector::PrepareFinalizersForResurrection()
{
    RootVisitor snapshotVisitor = [](ObjectRef&) {};
    snapshotFinalizerNum = collectorResources.VisitFinalizers(snapshotVisitor);

    std::function<bool(BaseObject*)> finalizable = [this](BaseObject* obj) {
        if (!IsTraceableObjectAddress(obj) || !obj->IsValidObject()) {
            return false;
        }
        return !IsMarkedObject(obj);
    };
    collectorResources.EnqueueFinalizables(finalizable, snapshotFinalizerNum);
}

void MarkSweepCollector::CollectResurrectionCandidatesToMain(GreyWorkQueue& mainQueue)
{
    VisitFinalizerRoots([&mainQueue, this](ObjectRef& ref) {
        BaseObject* finalizerObj = ref.object;
        if (IsTraceableObjectAddress(finalizerObj) && finalizerObj->IsValidObject()) {
            (void)TryEnqueueGreyObject(finalizerObj, mainQueue);
        }
    });
}

void MarkSweepCollector::ExecuteLikeDrainMainQueue(GreyWorkQueue& mainQueue)
{
    while (!mainQueue.empty()) {
        BaseObject* obj = mainQueue.back();
        mainQueue.pop_back();
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
            WeakRefBuffer::Instance().Insert(obj);
            continue;
        }

        TraceObjectRefFields(obj, mainQueue.UnsafeRaw());
    }

}

void MarkSweepCollector::ClearWeakRefsAfterMark()
{
    size_t cleared = 0;
    size_t preserved = 0;
    WeakRefBuffer::Instance().DrainWeakRefs([this, &cleared, &preserved](BaseObject* weakRef) {
        if (!IsTraceableObjectAddress(weakRef) || !weakRef->IsValidObject()) {
            return;
        }

        RefField<>* referentField = reinterpret_cast<RefField<>*>(
            reinterpret_cast<uintptr_t>(weakRef) + TYPEINFO_PTR_SIZE);
        BaseObject* referent = MSLoadRawReference(*referentField);
        if (referent == nullptr || !IsManagedAddress(reinterpret_cast<MAddress>(referent))) {
            return;
        }

        bool referentMarked = IsTraceableObjectAddress(referent) &&
            referent->IsValidObject() && IsMarkedObject(referent);
        if (!referentMarked) {
            MSStoreRawReference(*referentField, nullptr);
            cleared++;
            return;
        }
        preserved++;
    });
    VLOG(REPORT,
         "[MS-GC] ProcessWeakRefs(STW): cleared=%zu preserved=%zu",
         cleared,
         preserved);
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

    // 1. Finalizer reachability and resurrection were completed under STW.
    // PostMark only wakes the worker for the already-published finalizable set.
    {
        uint64_t t0 = TimeUtil::MicroSeconds();
        ProcessFinalizers();
        uint64_t finalizerUs = TimeUtil::MicroSeconds() - t0;
        VLOG(REPORT, "[MS-GC] Finalizers: snapshot=%u objects, notify=%sus",
             snapshotFinalizerNum, Pretty(finalizerUs).Str());
    }

    // 6. Large object garbage collection (eager sweep for in-heap large regions).
    CollectLargeGarbage();

    // 4. Publish the stable mark statistics and hand small-Region sweep over
    // to the background processor. MarkTermination has already restored IDLE
    // and installed the idle barrier before Mutators resumed, so this does not
    // change barrier semantics; it only makes the completed mark result visible
    // to HEU accounting and the asynchronous Sweep/Scavenge workers.
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    MSPostMarkSweepPlan sweepPlan = BuildMSPostMarkSweepPlan(gcReason == GC_REASON_OOM);
    if (centralPool != nullptr) {
        MSMarkedStats markedStats = centralPool->ComputeMarkedStats();
        cycleStats_.trueLiveBytes = markedStats.liveBytes;
        prevCycleMarkedObjects_.store(markedStats.objectCount, std::memory_order_relaxed);
        VLOG(REPORT, "[MS-GC] Mark bitmap: objects=%zu liveBytes=%zuB",
             markedStats.objectCount, markedStats.liveBytes);

        if (sweepPlan.parallelSweepOnceInPostMark) {
            RunOOMEmergencyParallelSweep(*centralPool);
        }
        if (sweepPlan.flushAllCaches) {
            size_t flushedEmptyBytes = space.FlushAllEmptySweptToHotFree();
            size_t flushedLargeBytes = space.FlushAllLargeHotCache();
            VLOG(REPORT,
                 "[MS-GC] OOM cache flush: empty=%zuB large=%zuB hotFree=%zuB",
                 flushedEmptyBytes, flushedLargeBytes, space.GetHotFreeBytesReference());
            centralPool->ReenableLazySweep();
        }
    }

    BackgroundProcessor& backgroundProcessor = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
    if (sweepPlan.notifyBackgroundSweep) {
        backgroundProcessor.NotifyToSweep(kNoPreferredSweepClass);
    }
    backgroundProcessor.NotifyToScavenge();

    cycleStats_.postMarkUs = TimeUtil::MicroSeconds() - phaseStart;
    VLOG(REPORT, "[MS-GC] PostMark total: %sus",
         Pretty(cycleStats_.postMarkUs).Str());
}

void MarkSweepCollector::RunOOMEmergencyParallelSweep(CentralRegionPool& centralPool)
{
    size_t unsweptBefore = centralPool.GetTotalUnsweptCount();
    size_t configuredThreads = std::max<int32_t>(1, CangjieRuntime::GetGCParam().gcThreads);
    size_t helperCount = ComputeMSMaxSweepHelpers(
        configuredThreads, CangjieRuntime::GetGCParam().msParallelSweepMaxHelpers);
    GCThreadPool* threadPool = GetThreadPool();
    if (threadPool == nullptr) {
        helperCount = 0;
    } else {
        helperCount = std::min(helperCount, static_cast<size_t>(threadPool->GetMaxThreadNum()));
    }

    bool poolOwned = helperCount != 0 && collectorResources.TryAcquireMSGCWorkerPoolForSweep();
    if (!poolOwned) {
        helperCount = 0;
    }

    std::vector<std::unique_ptr<HeapWork>> stagedTasks;
    stagedTasks.reserve(helperCount);
    bool allocationFailed = false;
    for (size_t taskIndex = 0; taskIndex < helperCount; ++taskIndex) {
        uint8_t preferredCls = static_cast<uint8_t>(
            (taskIndex + 1) % static_cast<size_t>(kNumSizeClasses));
        std::unique_ptr<HeapWork> task(new (std::nothrow) LambdaWork(
            [&centralPool, preferredCls](size_t) {
                centralPool.ParallelSweepOnceForOOM(preferredCls);
            }));
        if (task == nullptr) {
            allocationFailed = true;
            break;
        }
        stagedTasks.push_back(std::move(task));
    }

    if (allocationFailed) {
        stagedTasks.clear();
        helperCount = 0;
    } else if (helperCount != 0) {
        threadPool->SetMaxActiveThreadNum(static_cast<int32_t>(helperCount));
        for (std::unique_ptr<HeapWork>& task : stagedTasks) {
            threadPool->AddWork(task.release());
        }
        threadPool->Start();
    }

    // GC main participates in the same one-pass claim protocol. Every helper
    // task is joined before cache flush, but reaching zero unswept Regions is not
    // a completion requirement for this emergency pass.
    centralPool.ParallelSweepOnceForOOM(kNoPreferredSweepClass);
    if (helperCount != 0) {
        threadPool->WaitFinish();
    }
    if (poolOwned) {
        collectorResources.ReleaseMSGCWorkerPool(MSGCWorkerPoolOwner::SWEEP);
    }

    // This is the only residual observation in the OOM PostMark path. Do not
    // retry, assert zero, or publish sweepDone_; reopened ordinary Sweep lanes
    // may continue processing any residual work.
    size_t unsweptAfter = centralPool.GetTotalUnsweptCount();
    VLOG(REPORT,
         "[MS-GC] OOM parallel sweep once: helpers=%zu allocationFallback=%d unswept=%zu->%zu",
         helperCount, static_cast<int>(allocationFailed), unsweptBefore, unsweptAfter);
}

void MarkSweepCollector::ProcessFinalizers()
{
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
        centralPool->SweepLargeRegions();
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
            BaseObject* target = MSLoadRawReference(field);
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
    BaseObject* target = MSLoadRawReference(ref);
    const_cast<MarkSweepCollector*>(this)->DiscoverAndPushObject(target, rootSet);
}

void MarkSweepCollector::EnumAndTagRawRoot(ObjectRef& root, RootSet& rootSet) const
{
    // MS version: no tag/CAS, just push heap objects into root set
    BaseObject* obj = root.object;
    const_cast<MarkSweepCollector*>(this)->DiscoverAndPushObject(obj, rootSet);
}

void MarkSweepCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack)
{
    auto refFunc = [this, &workStack](RefField<>& field) {
        TraceRefField(field, workStack);
    };
    ForEachRefFieldGCInline(obj, refFunc);
}

BaseObject* MarkSweepCollector::GetAndTryTagObj(BaseObject* obj, RefField<>& field)
{
    // No tagging in Mark-Sweep
    (void)obj;
    BaseObject* target = MSLoadRawReference(field);
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
    return RefField<>(reinterpret_cast<MAddress>(obj));
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
    if (!IsMSGCReportEnabled()) {
        return;
    }
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
    if (!IsMSGCReportEnabled()) {
        return;
    }
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool == nullptr) {
        VLOG(REPORT, "[MS-GC] --- %s --- (no central pool)", stage);
        return;
    }

    size_t allocBufferCount = centralPool->GetAllocBufferCount();
    size_t bufferActiveRegionCount = centralPool->GetAllAllocBufferActiveRegionCount();
    size_t emptyCacheCount = centralPool->GetEmptySweptRegionCount();
    size_t largeRegionCount = centralPool->GetLargeRegionCount();

    VLOG(REPORT, "[MS-GC] --- %s ---", stage);
    VLOG(REPORT, "[MS-GC]   alloc buffers: %zu", allocBufferCount);
    VLOG(REPORT, "[MS-GC]   alloc-buffer active regions: %zu", bufferActiveRegionCount);
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
                           cycleStats_.postMarkUs + cycleStats_.postGcHookUs;
    uint64_t otherUs = (gcTimeUs > accountedUs) ? (gcTimeUs - accountedUs) : 0;

    VLOG(REPORT,
         "[MS-GC] total gc time: %s us (PreEnum %s + EnumRoots %s + ConcMark %s + "
         "MarkTerm %s + PostMark %s + PostGC %s + Other %s)",
         Pretty(gcTimeUs).Str(),
         Pretty(cycleStats_.preEnumRootsUs).Str(),
         Pretty(cycleStats_.markSetupUs).Str(),
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         Pretty(cycleStats_.markTerminationUs).Str(),
         Pretty(cycleStats_.postMarkUs).Str(),
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
    size_t oldLastStableLiveBytes = gcStats.lastStableLiveBytes;
    size_t previousInUseBytes = gcStats.lastRegionBytes;
    // Use mark-bit based live bytes (ComputeMarkedStats, computed in
    // PostMarkTermination). New allocations are not counted merely because
    // they were allocated, but a long SATB window can retain objects that
    // became unreachable during marking. Classify that floating-live shape
    // before allowing this sample to update the stable threshold baseline.
    size_t liveBytes = cycleStats_.trueLiveBytes;
    size_t inUseBytes = space.GetHeapInUseBytes();
    size_t maxHeapBytes = space.GetMaxCapacity();
    cycleStats_.markEndInUseBytes = inUseBytes;
    cycleStats_.inUseBurstBytes =
        inUseBytes > cycleStats_.markStartInUseBytes
        ? inUseBytes - cycleStats_.markStartInUseBytes
        : 0;
    bool exceptionalBypass = gcStats.reason == GC_REASON_OOM;
    MSStableLiveDecision stableDecision = EvaluateMSStableLive(
        heapThresholdState_,
        oldLastStableLiveBytes,
        cycleStats_.markStartInUseBytes,
        cycleStats_.markEndInUseBytes,
        liveBytes,
        exceptionalBypass);
    cycleStats_.floatingLiveInflated = stableDecision.floatingLiveInflated;
    cycleStats_.thresholdBaseBytes = stableDecision.thresholdBase;
    gcStats.lastStableLiveBytes = stableDecision.nextStableLive;
    gcStats.lastTrueLiveBytes = liveBytes;
    gcStats.lastRegionBytes = inUseBytes;
    gcStats.baselineAllocatedBytes = inUseBytes;

    // The raw MSGC target is medianStableLive * (1 + heapGrowth). Established
    // normal history uses an inclusive 10% deadband and symmetric 12.5%
    // transition limits. Bootstrap and OOM cycles establish their clamped
    // target directly.
    size_t baseBytes = stableDecision.thresholdBase;

    double heapGrowth = CangjieRuntime::GetHeapParam().heapGrowth;
    size_t gcThresholdCap = maxHeapBytes != 0
        ? maxHeapBytes
        : CangjieRuntime::GetGCParam().gcThreshold;
    // Floor to avoid GC thrash when base is tiny; cap at the allocator's actual
    // maximum heap rather than the initial committed heap.
    size_t minThreshold = std::min(gcThresholdCap, static_cast<size_t>(20) * 1024 * 1024);
    bool bypassTransition =
        exceptionalBypass || !stableDecision.hadAcceptedHistory;
    MSHeapThresholdDecision thresholdDecision =
        EvaluateMSHeapThreshold(
            oldThreshold, baseBytes, heapGrowth, minThreshold, gcThresholdCap,
            bypassTransition);
    size_t newThreshold = thresholdDecision.threshold;
    MSRegionManager* msRegionManager = space.GetMSRegionManager();
    size_t reusedScavengedRegionCount = msRegionManager != nullptr
        ? msRegionManager->GetReusedScavengedRegionCount()
        : previousReusedScavengedRegionCount_;
    size_t coldReuseRegions =
        reusedScavengedRegionCount >= previousReusedScavengedRegionCount_
        ? reusedScavengedRegionCount - previousReusedScavengedRegionCount_
        : reusedScavengedRegionCount;
    previousReusedScavengedRegionCount_ = reusedScavengedRegionCount;
    size_t coldReuseBytes = coldReuseRegions >
        std::numeric_limits<size_t>::max() / MS_REGION_SIZE
        ? std::numeric_limits<size_t>::max()
        : coldReuseRegions * MS_REGION_SIZE;
    MSScavengeThresholdDecision scavengeDecision = EvaluateMSScavengeThreshold(
        scavengeThresholdState_,
        cycleStats_.markStartInUseBytes,
        cycleStats_.markEndInUseBytes,
        coldReuseBytes,
        maxHeapBytes,
        oldScavengeThreshold,
        kMSShardSegmentBytes);
    size_t newScavengeThreshold = scavengeDecision.thresholdBytes;
    gcStats.SetThreshold(newThreshold);
    gcStats.SetScavengeThreshold(newScavengeThreshold);
    gcStats.lastSurvivingRegionBytes = liveBytes;
    previousAssistProcessedObjects_ = cycleStats_.assistProcessedObjects;
    previousBackgroundProcessedObjects_ = cycleStats_.gcMainProcessedObjects +
                                          cycleStats_.helperProcessedObjects;
    VLOG(REPORT,
         "[MS-GC] heapThreshold: oldThreshold=%zuB newThreshold=%zuB "
         "oldScavengeThreshold=%zuB newScavengeThreshold=%zuB "
         "prevLive=%zuB prevStableLive=%zuB trueLive=%zuB base=%zuB "
         "markStartInUse=%zuB markEndInUse=%zuB inUseBurst=%zuB "
         "floatingLiveInflated=%d acceptedSample=%d historySize=%zu "
         "medianStableLive=%zuB exceptionalBypass=%d "
         "growth=%.2f "
         "rawTarget=%zuB deadbandLower=%zuB deadbandUpper=%zuB deadbandHeld=%d "
         "riseLimit=%zuB fallLimit=%zuB riseLimited=%d fallLimited=%d "
         "transitionLimited=%d transitionBypassed=%d "
         "previousInUse=%zuB inUse=%zuB maxHeap=%zuB "
         "scavengeRecentPeakInUse=%zuB scavengeRecentBurst=%zuB "
         "scavengeColdReuseRegions=%zu scavengeColdReuse=%zuB "
         "scavengeResidentReserve=%zuB scavengeCandidate=%zuB "
         "scavengeSegment=%zuB scavengeNoColdReuseCycles=%zu "
         "scavengeDecreased=%d scavengeThresholdChangeLimited=%d",
         oldThreshold, newThreshold, oldScavengeThreshold, newScavengeThreshold,
         oldLastTrueLiveBytes, oldLastStableLiveBytes, liveBytes, baseBytes,
         cycleStats_.markStartInUseBytes, cycleStats_.markEndInUseBytes,
         cycleStats_.inUseBurstBytes,
         static_cast<int>(cycleStats_.floatingLiveInflated),
         static_cast<int>(stableDecision.acceptedSample),
         stableDecision.historySize, stableDecision.medianStableLive,
         static_cast<int>(stableDecision.exceptionalBypass),
         1.0 + heapGrowth, thresholdDecision.rawTarget,
         thresholdDecision.deadbandLower, thresholdDecision.deadbandUpper,
         static_cast<int>(thresholdDecision.deadbandHeld),
         thresholdDecision.riseLimit, thresholdDecision.fallLimit,
         static_cast<int>(thresholdDecision.riseLimited),
         static_cast<int>(thresholdDecision.fallLimited),
         static_cast<int>(thresholdDecision.transitionLimited),
         static_cast<int>(thresholdDecision.transitionBypassed),
         previousInUseBytes, inUseBytes, maxHeapBytes,
         scavengeDecision.recentPeakInUseBytes, scavengeDecision.recentBurstBytes,
         coldReuseRegions, scavengeDecision.coldReuseBytes,
         scavengeDecision.residentReserveBytes, scavengeDecision.candidateBytes,
         kMSShardSegmentBytes,
         scavengeDecision.noColdReuseCycles,
         static_cast<int>(scavengeDecision.decreased),
         static_cast<int>(scavengeDecision.thresholdChangeLimited));

    VLOG(REPORT,
         "[MS-GC] live bytes %zu, inUseRegionBytes %zu, update gc threshold %zu -> %zu",
         liveBytes, inUseBytes, oldThreshold, gcStats.GetThreshold());
    uint64_t updateStatsUs = TimeUtil::MicroSeconds() - updateStatsStart;
    VLOG(REPORT, "[MS-GC] UpdateGCStats took %lu us", static_cast<unsigned long>(updateStatsUs));
    TRACE_COUNT("CJRT_post_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
