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
constexpr uint64_t kMSMarkProgressWaitUs = 50;

struct MSNoopWorkUnitCounter {
    void AddResolvedRefs(uint64_t = 1)
    {
    }

    void AddMarkWordRMW(uint64_t = 1)
    {
    }
};

bool IsMSGCReportEnabled()
{
#if defined(__OHOS__)
    return true;
#else
    return ENABLE_LOG(REPORT);
#endif
}

class MSWeakRefLocalBatch {
public:
    static constexpr size_t kCapacity = 32;

    ~MSWeakRefLocalBatch()
    {
        CHECK_DETAIL(count_ == 0,
                     "[MS-GC] destroying non-empty WeakRef local batch");
    }

    void Add(BaseObject* obj)
    {
        refs_[count_++] = obj;
        if (count_ == kCapacity) {
            Flush();
        }
    }

    void Flush()
    {
        WeakRefBuffer::Instance().InsertBatch(refs_, count_);
        count_ = 0;
    }

private:
    BaseObject* refs_[kCapacity] = {};
    size_t count_ = 0;
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

static inline bool IsOutsideCurrentCollectSetObject(BaseObject* obj, uint32_t currentMarkEpoch)
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

    if (msInfo->GetRegionKind() == MSRegionKind::SPAN) {
        msInfo = MSRegionInfoArray::ResolveLargeBaseInfo(msInfo);
    }
    if (msInfo == nullptr ||
        (msInfo->GetRegionKind() != MSRegionKind::SMALL &&
         msInfo->GetRegionKind() != MSRegionKind::LARGE)) {
        return false;
    }
    return msInfo->collectEpoch != currentMarkEpoch;
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

    if (msInfo->IsLargeRegionEntry()) {
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

    if (msInfo->IsFreeRegion()) {
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
    if (slotIdx >= msInfo->GetTotalSlotCount()) {
        return false;
    }
    if (msInfo->GetAllocMode() == MSAllocMode::BUMP && addr >= msInfo->allocPtr) {
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

    if (info->IsLargeRegionEntry()) {
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

    if (info->IsFreeRegion() ||
        info->sizeClass >= kNumSizeClasses ||
        info->GetTotalSlotCount() == 0) {
        return false;
    }

    size_t slotSize = info->GetSlotSize();
    if (slotSize == 0) {
        return false;
    }

    size_t slotIdx = SizeClassInfo::OffsetToSlotIndex(info->sizeClass, regionOffset);
    if (slotIdx >= info->GetTotalSlotCount() || slotIdx * slotSize != regionOffset) {
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

static inline uint32_t GetOrInitBatchMarkEpoch(MSMarkWordBatch& batch)
{
    if (LIKELY(batch.HasMarkEpoch())) {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        CHECK_DETAIL(batch.HasBitmapPlaneBases(),
                     "[MS-GC] MarkWordBatch epoch without bitmap planes");
#endif
        return batch.GetMarkEpoch();
    }
    uint32_t markEpoch = __atomic_load_n(
        &CJ_MRT_MSGCFastPathState.currentMarkEpoch, __ATOMIC_ACQUIRE);
    batch.SetMarkEpoch(markEpoch);
    batch.SetBitmapPlaneBases(
        static_cast<uintptr_t>(__atomic_load_n(
            &CJ_MRT_MSGCFastPathState.allocPlaneBase, __ATOMIC_ACQUIRE)),
        static_cast<uintptr_t>(__atomic_load_n(
            &CJ_MRT_MSGCFastPathState.markPlaneBase, __ATOMIC_ACQUIRE)));
    return markEpoch;
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
    cycleCompletedMarkWU_.store(0, std::memory_order_relaxed);
    cycleAllocatedMarkBytes_.store(0, std::memory_order_relaxed);
    assistWorkPerByteScaled_.store(0, std::memory_order_relaxed);

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
            markScheduler_.ResetCycle();
            PrepareBarrierMarkBuffersForNewEpochUnderSTW(*centralPool);
            centralPool->BuildCollectSet();
            uint64_t clearBitmapT0 = TimeUtil::MicroSeconds();
            centralPool->ClearAllMarkBitmaps();
            clearBitmapUs = TimeUtil::MicroSeconds() - clearBitmapT0;
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
    rootStaging_.clear();
    {
        EnumAndMarkRoots(rootStaging_.UnsafeRaw());
        cycleStats_.rootsCount = rootStaging_.size();
        PublishRootStaging();
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

template<typename WorkCounter>
void MarkSweepCollector::AddRefToMarkBatchImpl(
    BaseObject* obj, bool knownManaged, MSMarkWordBatch& batch,
    WorkCounter& work)
{
    ResolvedMarkTarget target;
    bool resolved = knownManaged
        ? ResolveManagedMarkTarget(obj, target)
        : ResolveMarkTarget(obj, target);
    if (UNLIKELY(!resolved)) {
        return;
    }
    work.AddResolvedRefs();

    // LARGE/SPAN has one canonical mark flag and one OBJECT task. It is not
    // mixed into the small-object word batch.
    if (target.isLarge) {
        if (LIKELY(target.IsMarked())) {
            return;
        }
        TypeInfo* typeInfo = obj->GetTypeInfo();
        if (UNLIKELY(typeInfo == nullptr)) {
            return;
        }
        work.AddMarkWordRMW();
        if (!target.info->AtomicTestAndSetMarked() &&
            target.info->GetScanClass() == MSScanClass::HAS_REF) {
            if (markScheduler_.PublishObject(target.info)) {
                markWorkEpoch_.Notify();
            }
        }
        return;
    }

    uint32_t currentEpoch = GetOrInitBatchMarkEpoch(batch);
    uint32_t regionIdx =
        static_cast<uint32_t>(MSRegionInfoArray::GetRegionIdx(target.info));
    uint64_t* markWord =
        &batch.GetMarkBitmap(regionIdx)[target.wordIdx];
    // Mark bits are monotonic for the lifetime of one batch/GC epoch. Avoid
    // entering the coalescing cache and executing an atomic RMW for the common
    // shared/cyclic-graph case where another edge already discovered target.
    if (LIKELY(
            (__atomic_load_n(markWord, __ATOMIC_RELAXED) &
             target.bitMask) != 0)) {
        return;
    }

    bool inCollectSet =
        currentEpoch != MSRegionInfo::NO_COLLECT_EPOCH &&
        target.info->collectEpoch == currentEpoch;
    if (!inCollectSet) {
        // OUTSIDE_SET ALLOC may still be owned and modified by a mutator. Do
        // not read it. The exact slot coordinate was checked above; validate
        // object publication through stateWord/TypeInfo before caching the bit.
        TypeInfo* typeInfo = obj->GetTypeInfo();
        if (UNLIKELY(typeInfo == nullptr)) {
            return;
        }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        if (target.info->GetScanClass() == MSScanClass::NO_REF) {
            CHECK_DETAIL(!typeInfo->HasRefField(),
                         "[MS-GC] NO_REF Region contains HAS_REF object obj=%p idx=%zu",
                         static_cast<void*>(obj),
                         MSRegionInfoArray::GetRegionIdx(target.info));
        }
#endif
    }

    batch.Add(regionIdx, target.wordIdx, target.bitMask,
              [this, &work, &batch](const MSMarkWordBatchEntry& entry) {
                  FlushMarkWordEntryImpl(entry, work, batch);
              });
}

template<typename WorkCounter>
void MarkSweepCollector::FlushMarkWordEntryImpl(
    const MSMarkWordBatchEntry& entry, WorkCounter& work,
    const MSMarkWordBatch& batch)
{
    MSRegionInfo* info =
        MSRegionInfoArray::GetRegionInfoByIdx(entry.regionIdx);
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    CHECK_DETAIL(
        info->GetRegionKind() == MSRegionKind::SMALL &&
        entry.wordIdx < info->GetBitmapWordCount(),
        "[MS-GC] stale MarkWordBatch entry region=%u word=%u kind=%u words=%u",
        entry.regionIdx, static_cast<unsigned>(entry.wordIdx),
        static_cast<unsigned>(info->GetRegionKind()),
        static_cast<unsigned>(info->GetBitmapWordCount()));
#endif

    uint64_t mask = entry.mask;
    uint32_t markEpoch = batch.GetMarkEpoch();
    if (markEpoch != MSRegionInfo::NO_COLLECT_EPOCH &&
        info->collectEpoch == markEpoch) {
        uint64_t validMask =
            entry.wordIdx + 1 == info->GetBitmapWordCount()
            ? info->GetValidTailMask()
            : ~UINT64_C(0);
        uint64_t allocWord =
            __atomic_load_n(&batch.GetAllocBitmap(entry.regionIdx)[entry.wordIdx],
                            __ATOMIC_RELAXED);
        mask &= ~allocWord & validMask;
    }
    if (mask == 0) {
        return;
    }

    uint64_t* markWord = &batch.GetMarkBitmap(entry.regionIdx)[entry.wordIdx];
    mask &= ~__atomic_load_n(markWord, __ATOMIC_RELAXED);
    if (mask == 0) {
        return;
    }

    work.AddMarkWordRMW();
    uint64_t old = __atomic_fetch_or(markWord, mask, __ATOMIC_RELAXED);
    uint64_t newBits = mask & ~old;
    if (newBits == 0 || info->GetScanClass() == MSScanClass::NO_REF) {
        return;
    }

    info->PublishPendingMarkWord(entry.wordIdx);
    if (markScheduler_.TryPublishRegion(info)) {
        markWorkEpoch_.Notify();
    }
}

template<typename WorkCounter>
void MarkSweepCollector::FlushMarkBatchImpl(
    MSMarkWordBatch& batch, WorkCounter& work)
{
    if (batch.IsEmpty()) {
        return;
    }
    batch.FlushAll([this, &work, &batch](const MSMarkWordBatchEntry& entry) {
        FlushMarkWordEntryImpl(entry, work, batch);
    });
}

void MarkSweepCollector::AddRefToMarkBatch(
    BaseObject* obj, bool knownManaged, MSMarkWordBatch& batch,
    MSWorkUnitCounter& work)
{
    AddRefToMarkBatchImpl(obj, knownManaged, batch, work);
}

void MarkSweepCollector::AddManagedRefToBarrierBuffer(
    BaseObject* ref, MSBarrierMarkBuffer& buffer,
    uint32_t currentMarkEpoch)
{
    ResolvedMarkTarget target;
    if (UNLIKELY(!ResolveManagedMarkTarget(ref, target))) {
        return;
    }

    // LARGE/SPAN retains its canonical mark flag and OBJECT publication. It
    // does not consume a Small-word entry or force existing entries to flush.
    if (target.isLarge) {
        if (LIKELY(target.IsMarked())) {
            return;
        }
        TypeInfo* typeInfo = ref->GetTypeInfo();
        if (UNLIKELY(typeInfo == nullptr)) {
            return;
        }
        if (!target.info->AtomicTestAndSetMarked() &&
            target.info->GetScanClass() == MSScanClass::HAS_REF) {
            if (markScheduler_.PublishObject(target.info)) {
                markWorkEpoch_.Notify();
            }
        }
        return;
    }

    if (UNLIKELY(
            buffer.pendingBitCount >=
            kMSBarrierMarkBufferPendingBitFlushThreshold)) {
        FlushBarrierMarkBuffer(buffer);
    }

    uint32_t regionIdx = static_cast<uint32_t>(
        MSRegionInfoArray::GetRegionIdx(target.info));
    uint64_t wordKey = MakeMSBarrierMarkWordKey(
        regionIdx, static_cast<uint32_t>(target.wordIdx));
    MSBarrierMarkLookup localLookup =
        LookupMSBarrierMarkEntry(buffer, wordKey);
    uint8_t localEntry = localLookup.existingEntry;
    if (localEntry != kMSBarrierMarkBufferInvalidEntry &&
        (buffer.entries[localEntry].mask & target.bitMask) != 0) {
        buffer.lastEntry = localEntry;
        return;
    }

    bool inCollectSet =
        currentMarkEpoch != MSRegionInfo::NO_COLLECT_EPOCH &&
        target.info->collectEpoch == currentMarkEpoch;
    if (!inCollectSet) {
        // OUTSIDE_SET allocation metadata can still be owner-mutated. Do not
        // read its alloc plane; validate publication before caching a new bit.
        TypeInfo* typeInfo = ref->GetTypeInfo();
        if (UNLIKELY(typeInfo == nullptr)) {
            return;
        }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        if (target.info->GetScanClass() == MSScanClass::NO_REF) {
            CHECK_DETAIL(
                !typeInfo->HasRefField(),
                "[MS-GC] NO_REF Region contains HAS_REF object obj=%p idx=%u",
                static_cast<void*>(ref), regionIdx);
        }
#endif
    }

    MSBarrierMarkAddResult result = TryAddMSBarrierMarkSingleBitAtLookup(
        buffer, wordKey, target.bitMask, localLookup);
    if (LIKELY(result != MSBarrierMarkAddResult::NEEDS_FLUSH)) {
        return;
    }

    FlushBarrierMarkBuffer(buffer);
    result = TryAddMSBarrierMarkSingleBitAtLookup(
        buffer, wordKey, target.bitMask,
        LookupMSBarrierMarkEntry(buffer, wordKey));
    CHECK_DETAIL(
        result != MSBarrierMarkAddResult::NEEDS_FLUSH,
        "[MS-GC] empty barrier mark buffer rejected one bit key=0x%llx",
        static_cast<unsigned long long>(wordKey));
}

void MarkSweepCollector::PublishManagedRefForBarrierImmediately(
    BaseObject* ref)
{
    MSMarkWordBatch batch;
    MSNoopWorkUnitCounter noWork;
    AddRefToMarkBatchImpl(ref, true, batch, noWork);
    FlushMarkBatchImpl(batch, noWork);
}

void MarkSweepCollector::FlushBarrierMarkBuffer(
    MSBarrierMarkBuffer& buffer)
{
    if (IsMSBarrierMarkBufferEmpty(buffer)) {
        return;
    }

    uint32_t currentEpoch = __atomic_load_n(
        &CJ_MRT_MSGCFastPathState.currentMarkEpoch, __ATOMIC_ACQUIRE);
    uintptr_t allocPlaneBase = static_cast<uintptr_t>(__atomic_load_n(
        &CJ_MRT_MSGCFastPathState.allocPlaneBase, __ATOMIC_ACQUIRE));
    uintptr_t markPlaneBase = static_cast<uintptr_t>(__atomic_load_n(
        &CJ_MRT_MSGCFastPathState.markPlaneBase, __ATOMIC_ACQUIRE));
    CHECK_DETAIL(
        allocPlaneBase != 0 && markPlaneBase != 0,
        "[MS-GC] barrier flush without published bitmap planes");

    struct PendingRegionPublication {
        MSRegionInfo* info;
        uint64_t wordMask;
    };
    PendingRegionPublication pending[kMSBarrierMarkBufferCapacity];
    size_t pendingCount = 0;

    uint32_t occupied = buffer.occupancy;
    while (occupied != 0) {
        uint32_t entryIdx =
            static_cast<uint32_t>(__builtin_ctz(occupied));
        occupied &= occupied - 1;
        const MSBarrierMarkEntry& entry = buffer.entries[entryIdx];
        uint32_t regionIdx = GetMSBarrierMarkRegionIndex(entry.wordKey);
        uint32_t wordIdx = GetMSBarrierMarkWordIndex(entry.wordKey);
        CHECK_DETAIL(
            regionIdx < MSRegionInfoArray::GetMaxRegionCount(),
            "[MS-GC] barrier entry Region index overflow: %u", regionIdx);
        MSRegionInfo* info =
            MSRegionInfoArray::GetRegionInfoByIdx(regionIdx);
        CHECK_DETAIL(
            info->GetRegionKind() == MSRegionKind::SMALL &&
            wordIdx < info->GetBitmapWordCount(),
            "[MS-GC] stale barrier entry region=%u word=%u kind=%u words=%u",
            regionIdx, wordIdx,
            static_cast<unsigned>(info->GetRegionKind()),
            static_cast<unsigned>(info->GetBitmapWordCount()));

        uint64_t mask = entry.mask;
        if (currentEpoch != MSRegionInfo::NO_COLLECT_EPOCH &&
            info->collectEpoch == currentEpoch) {
            uint64_t validMask =
                wordIdx + 1 == info->GetBitmapWordCount()
                ? info->GetValidTailMask()
                : ~UINT64_C(0);
            uint64_t* allocWord =
                reinterpret_cast<uint64_t*>(allocPlaneBase) + entry.wordKey;
            uint64_t allocated =
                __atomic_load_n(allocWord, __ATOMIC_RELAXED);
            mask &= ~allocated & validMask;
        }
        if (mask == 0) {
            continue;
        }

        uint64_t* markWord =
            reinterpret_cast<uint64_t*>(markPlaneBase) + entry.wordKey;
        mask &= ~__atomic_load_n(markWord, __ATOMIC_RELAXED);
        if (mask == 0) {
            continue;
        }
        uint64_t old =
            __atomic_fetch_or(markWord, mask, __ATOMIC_RELAXED);
        uint64_t newBits = mask & ~old;
        if (newBits == 0 ||
            info->GetScanClass() == MSScanClass::NO_REF) {
            continue;
        }

        size_t publicationIdx = 0;
        while (publicationIdx < pendingCount &&
               pending[publicationIdx].info != info) {
            ++publicationIdx;
        }
        if (publicationIdx == pendingCount) {
            pending[pendingCount++] =
                PendingRegionPublication {info, 0};
        }
        pending[publicationIdx].wordMask |= UINT64_C(1) << wordIdx;
    }

    for (size_t i = 0; i < pendingCount; ++i) {
        pending[i].info->PublishPendingMarkWords(pending[i].wordMask);
        if (markScheduler_.TryPublishRegion(pending[i].info)) {
            markWorkEpoch_.Notify();
        }
    }

    ClearMSBarrierMarkPendingBits(buffer);
}

void MarkSweepCollector::FlushMarkBatch(
    MSMarkWordBatch& batch, MSWorkUnitCounter& work)
{
    FlushMarkBatchImpl(batch, work);
}

void MarkSweepCollector::NotifyAllocBufferExit(AllocBufferMS& allocBuffer)
{
    FlushBarrierMarkBuffer(allocBuffer.GetBarrierMarkBuffer());
}

void MarkSweepCollector::FlushAllBarrierMarkBuffersUnderSTW(
    CentralRegionPool& central)
{
    central.VisitAllocBufferMS([this](AllocBufferMS& buffer) {
        FlushBarrierMarkBuffer(buffer.GetBarrierMarkBuffer());
    });
}

void MarkSweepCollector::PrepareBarrierMarkBuffersForNewEpochUnderSTW(
    CentralRegionPool& central)
{
    central.VisitAllocBufferMS([](AllocBufferMS& buffer) {
        MSBarrierMarkBuffer& markBuffer = buffer.GetBarrierMarkBuffer();
        // The buffer has no epoch tag. Anything left while the mark barrier is
        // inactive belongs to the completed cycle and must not be published
        // into the mark plane that BuildCollectSet installs for the new cycle.
        ResetMSBarrierMarkBuffer(markBuffer);
    });
}

void MarkSweepCollector::RunRegionTask(
    MSTaskLease& lease, MSWorkUnitCounter& work, uint64_t taskBudgetWU)
{
    MSRegionInfo* info = lease.GetRegionInfo();
    CHECK_DETAIL(
        info != nullptr &&
        info->GetRegionKind() == MSRegionKind::SMALL &&
        info->GetScanClass() == MSScanClass::HAS_REF &&
        info->GetScanState() == MSScanState::OWNED,
        "[MS-GC] invalid Region task lease idx=%u", lease.GetRegionIdx());

    MSMarkWordBatch batch;
    uint32_t currentEpoch = GetOrInitBatchMarkEpoch(batch);
    bool inCollectSet =
        currentEpoch != MSRegionInfo::NO_COLLECT_EPOCH &&
        info->collectEpoch == currentEpoch;
    const size_t bitmapWordCount = info->GetBitmapWordCount();
    const uint64_t validTailMask = info->GetValidTailMask();
    uint32_t regionIdx = lease.GetRegionIdx();
    uint64_t* markBitmap = batch.GetMarkBitmap(regionIdx);
    uint64_t* scanBitmap = info->GetScanBitmap();
    uint64_t* allocBitmap =
        inCollectSet ? batch.GetAllocBitmap(regionIdx) : nullptr;
    const uintptr_t regionStart = info->RegionStart();
    const size_t slotSize = info->GetSlotSize();
    MSWeakRefLocalBatch weakRefs;
    bool budgetExpired = false;
    for (;;) {
        uint64_t summary = info->TakePendingMarkWords();
        while (summary != 0) {
            size_t wordIdx = static_cast<size_t>(__builtin_ctzll(summary));
            summary &= summary - 1;

            uint64_t markWord =
                __atomic_load_n(&markBitmap[wordIdx],
                                __ATOMIC_RELAXED);
            uint64_t oldScanWord = scanBitmap[wordIdx];
            uint64_t scanWord = oldScanWord;
            uint64_t candidates = markWord & ~scanWord;
            if (inCollectSet) {
                uint64_t validMask =
                    wordIdx + 1 == bitmapWordCount
                    ? validTailMask
                    : ~UINT64_C(0);
                uint64_t allocWord =
                    __atomic_load_n(&allocBitmap[wordIdx],
                                    __ATOMIC_RELAXED);
                candidates &= ~allocWord & validMask;
            }
            work.AddScanWords();

            while (candidates != 0) {
                size_t bitIdx =
                    static_cast<size_t>(__builtin_ctzll(candidates));
                uint64_t bit = UINT64_C(1) << bitIdx;
                candidates &= candidates - 1;

                size_t slotIdx = wordIdx * 64 + bitIdx;
                BaseObject* obj = reinterpret_cast<BaseObject*>(
                    regionStart + slotIdx * slotSize);
                TypeInfo* typeInfo = obj->GetTypeInfo();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
                CHECK_DETAIL(
                    typeInfo != nullptr,
                    "[MS-GC] Region owner found invalid marked object obj=%p "
                    "region=%u word=%zu bit=%zu",
                    static_cast<void*>(obj), lease.GetRegionIdx(),
                    wordIdx, bitIdx);
#endif

                if (UNLIKELY(obj->IsWeakRef())) {
                    weakRefs.Add(obj);
                } else if (LIKELY(typeInfo->HasRefField())) {
                    // HAS_REF is a Region allocation class, not a per-object
                    // guarantee. Finalizable and conservatively classified
                    // objects may legitimately have no reference fields.
                    auto refVisitor =
                        [this, &batch, &work](RefField<>& field) {
                            AddRefToMarkBatch(
                                MSLoadRawReference(field), false, batch, work);
                        };
                    ForEachRefFieldGCKnownType(
                        obj, typeInfo, refVisitor);
                }
                // Only this Region owner writes SCAN. Child publications may
                // still be local to batch, but the active lease keeps global
                // termination from observing a false fixed point.
                scanWord |= bit;
            }
            if (scanWord != oldScanWord) {
                scanBitmap[wordIdx] = scanWord;
            }

            if (work.Total() >= taskBudgetWU && summary != 0) {
                __atomic_fetch_or(
                    &info->pendingWordSummary, summary, __ATOMIC_RELEASE);
                budgetExpired = true;
                break;
            }
        }

        FlushMarkBatch(batch, work);
        weakRefs.Flush();
        if (budgetExpired) {
            break;
        }
        CHECK_DETAIL(
            info->CasScanState(MSScanState::OWNED, MSScanState::IDLE),
            "[MS-GC] Region owner release failed idx=%u state=%u",
            lease.GetRegionIdx(),
            static_cast<unsigned>(info->GetScanState()));

        // Producer publication is summary-first. If it raced the final owner
        // drain while this Region was OWNED, its TryPublishRegion() fast path
        // could not enqueue a token. Once IDLE, immediately republish any
        // such residual summary before completing this lease. A producer that
        // arrives after this load observes IDLE and publishes for itself; the
        // MarkTermination reconciliation is the final STW safety net.
        if (info->LoadPendingMarkWordsAcquire() != 0 &&
            markScheduler_.TryPublishRegion(info)) {
            markWorkEpoch_.Notify();
        }
        lease.Complete(batch.IsEmpty());
        return;
    }

    CHECK_DETAIL(
        budgetExpired &&
            info->LoadPendingMarkWordsAcquire() != 0,
        "[MS-GC] expired Region owner without pending work idx=%u",
        lease.GetRegionIdx());
    if (markScheduler_.RequeueOwnedRegion(info)) {
        markWorkEpoch_.Notify();
    }
    lease.Complete(batch.IsEmpty());
}

void MarkSweepCollector::RunObjectTask(
    MSTaskLease& lease, MSWorkUnitCounter& work)
{
    MSRegionInfo* info = lease.GetRegionInfo();
    CHECK_DETAIL(
        info != nullptr &&
        info->GetRegionKind() == MSRegionKind::LARGE &&
        info->GetScanClass() == MSScanClass::HAS_REF &&
        info->IsMarked(),
        "[MS-GC] invalid Object task lease idx=%u", lease.GetRegionIdx());

    BaseObject* obj =
        reinterpret_cast<BaseObject*>(info->LargeObjectStart());
    TypeInfo* typeInfo = obj->GetTypeInfo();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    CHECK_DETAIL(typeInfo != nullptr,
                 "[MS-GC] Object task has invalid TypeInfo obj=%p idx=%u",
                 static_cast<void*>(obj), lease.GetRegionIdx());
#endif

    MSMarkWordBatch batch;
    (void)GetOrInitBatchMarkEpoch(batch);
    MSWeakRefLocalBatch weakRefs;
    if (UNLIKELY(obj->IsWeakRef())) {
        weakRefs.Add(obj);
    } else {
        auto refVisitor = [this, &batch, &work](RefField<>& field) {
            AddRefToMarkBatch(
                MSLoadRawReference(field), false, batch, work);
        };
        ForEachRefFieldGCKnownType(obj, typeInfo, refVisitor);
    }
    FlushMarkBatch(batch, work);
    weakRefs.Flush();
    lease.Complete(batch.IsEmpty());
}

void MarkSweepCollector::RunSchedulerTask(
    MSTaskLease& lease, MSWorkUnitCounter& work, uint64_t taskBudgetWU)
{
    if (lease.GetKind() == MSMarkTaskKind::REGION) {
        RunRegionTask(lease, work, taskBudgetWU);
    } else {
        RunObjectTask(lease, work);
    }
}

uint64_t MarkSweepCollector::DrainSchedulerUnderSTW()
{
    uint64_t completedWU = 0;
    while (markScheduler_.OutstandingForTermination() != 0) {
        MSTaskLease lease;
        bool claimed = markScheduler_.TryClaim(
            MSMarkWorkerCaps::GCWorker(), true, lease);
        CHECK_DETAIL(
            claimed,
            "[MS-GC] TaskLedger nonzero without ready work under STW: "
            "outstanding=%llu",
            static_cast<unsigned long long>(
                markScheduler_.OutstandingForTermination()));
        MSWorkUnitCounter work;
        RunSchedulerTask(
            lease, work, std::numeric_limits<uint64_t>::max());
        completedWU = SaturatingMSWUAdd(completedWU, work.Total());
    }
    CHECK_DETAIL(
        markScheduler_.ReadyEmptyForTermination(),
        "[MS-GC] ready stack non-empty with zero TaskLedger");
    cycleCompletedMarkWU_.fetch_add(completedWU, std::memory_order_relaxed);
    return completedWU;
}

void MarkSweepCollector::VerifyMarkSchedulerQuiescence(
    const char* closureName)
{
    if (!IsVerifyMarkEnabled()) {
        return;
    }

    constexpr size_t kMaxReport = 32;
    size_t badRegions = 0;
    size_t strandedPendingRegions = 0;
    size_t nonIdleRegions = 0;
    size_t unscannedObjects = 0;
    size_t reported = 0;
    const size_t regionCount = MSRegionInfoArray::GetMaxRegionCount();
    for (size_t idx = 0; idx < regionCount; ++idx) {
        MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoByIdx(idx);
        if (info->GetRegionKind() != MSRegionKind::SMALL ||
            info->GetScanClass() != MSScanClass::HAS_REF) {
            continue;
        }

        uint64_t pending = info->LoadPendingMarkWordsAcquire();
        MSScanState state = info->GetScanState();
        size_t regionUnscanned = 0;
        uint64_t* markBitmap = info->GetMarkBitmap();
        uint64_t* scanBitmap = info->GetScanBitmap();
        for (size_t wordIdx = 0;
             wordIdx < info->GetBitmapWordCount(); ++wordIdx) {
            uint64_t markWord =
                __atomic_load_n(&markBitmap[wordIdx], __ATOMIC_RELAXED);
            regionUnscanned += static_cast<size_t>(
                __builtin_popcountll(markWord & ~scanBitmap[wordIdx]));
        }

        if (pending == 0 && state == MSScanState::IDLE &&
            regionUnscanned == 0) {
            continue;
        }
        ++badRegions;
        strandedPendingRegions += pending != 0 ? 1 : 0;
        nonIdleRegions += state != MSScanState::IDLE ? 1 : 0;
        unscannedObjects += regionUnscanned;
        if (reported < kMaxReport) {
            VLOG(REPORT,
                 "[MS-GC][VERIFY] scheduler-not-quiescent: closure=%s "
                 "region=%zu state=%u pending=0x%llx "
                 "unscannedObjects=%zu",
                 closureName, idx, static_cast<unsigned>(state),
                 static_cast<unsigned long long>(pending),
                 regionUnscanned);
            ++reported;
        }
    }

    VLOG(REPORT,
         "[MS-GC][VERIFY] scheduler-quiescence: closure=%s badRegions=%zu "
         "pendingRegions=%zu nonIdleRegions=%zu "
         "unscannedObjects=%zu",
         closureName, badRegions, strandedPendingRegions,
         nonIdleRegions, unscannedObjects);
    CHECK_DETAIL(
        badRegions == 0,
         "[MS-GC] Mark scheduler reached false fixed point: closure=%s "
         "badRegions=%zu pendingRegions=%zu "
         "nonIdleRegions=%zu unscannedObjects=%zu",
         closureName, badRegions, strandedPendingRegions,
         nonIdleRegions, unscannedObjects);
}

void MarkSweepCollector::EnumAndMarkRoots(WorkStack& workStack)
{
    // Root enumeration only stages raw candidates. One GC-owned MarkWordBatch
    // below performs the white->grey transition and task publication.
    EnumAllCommonRootsSerial(workStack);
    EnumAllExportRoots(workStack);
}

void MarkSweepCollector::PublishRootStaging()
{
    MSMarkWordBatch batch;
    MSWorkUnitCounter work;
    while (!rootStaging_.empty()) {
        BaseObject* obj = rootStaging_.back();
        rootStaging_.pop_back();
        AddRefToMarkBatch(obj, false, batch, work);
    }
    FlushMarkBatch(batch, work);
    cycleCompletedMarkWU_.fetch_add(work.Total(), std::memory_order_relaxed);
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
            workStack.push_back(obj);
        }
    }
}

// ============================================================================
// Step 2.2: ConcurrentMark
// ============================================================================

void MarkSweepCollector::ConcurrentMark()
{
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_CONCURRENT_MARK, true);
    markWorkerControl_.Start(0);

    bool disableAssist = ReadBoolParameter("CJ_MS_DISABLE_ASSIST", true);
    uint64_t assistRatio = ApplyMSAssistDisableOverride(
        ComputeMSAssistRatioQ16(
            markPacer_.GetWorkDensityQ16(), MSMarkPressure::NORMAL,
            markScheduler_.OutstandingHint()),
        disableAssist);
    assistWorkPerByteScaled_.store(assistRatio, std::memory_order_release);
    VLOG(REPORT,
         "[MS-GC] Assist pacing: disabled=%d effectiveRatio=%llu",
         static_cast<int>(disableAssist),
         static_cast<unsigned long long>(assistRatio));

    RunSchedulerMarkLoop();
    assistWorkPerByteScaled_.store(0, std::memory_order_release);
}

void MarkSweepCollector::RunSchedulerMarkLoop()
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    VLOG(REPORT,
         "[MS-GC] --- ConcurrentMark (REGION/OBJECT scheduler) ---");

    size_t configuredThreads =
        std::max<int32_t>(1, CangjieRuntime::GetGCParam().gcThreads);
    size_t maxHelpers = configuredThreads > 1 ? configuredThreads - 1 : 0;
    GCThreadPool* threadPool = GetThreadPool();
    if (threadPool == nullptr) {
        maxHelpers = 0;
    } else {
        maxHelpers = std::min(
            maxHelpers, static_cast<size_t>(threadPool->GetMaxThreadNum()));
    }

    bool markPoolOwned = false;
    if (maxHelpers != 0) {
        markPoolOwned = collectorResources.AcquireMSGCWorkerPoolForMark();
        if (!markPoolOwned) {
            maxHelpers = 0;
        }
    }

    MSMarkPressure pressure =
        gcReason == GC_REASON_OOM
        ? MSMarkPressure::CRITICAL
        : MSMarkPressure::NORMAL;
    size_t activeHelpers = ComputeMSMarkHelperTarget(
        markScheduler_.OutstandingHint(),
        maxHelpers, pressure);
    StartMarkWorkers(maxHelpers, activeHelpers);

    uint64_t markBudgetNs = CangjieRuntime::GetGCParam().msMarkTimeBudget;
    uint64_t markBudgetUs = markBudgetNs == 0
        ? 0
        : std::max<uint64_t>(1, markBudgetNs / 1000);
    uint64_t mainBudgetWU = ComputeMSMarkSliceBudget(
        markPacer_.GetThroughputQ16(), MS_WU_MAIN_WALL_BUDGET_US);
    for (;;) {
        uint64_t observedProgressEpoch = markWorkEpoch_.Load();
        MSMarkSliceResult slice = markScheduler_.RunMarkSlice(
            MSMarkWorkerCaps::GCWorker(), mainBudgetWU,
            MS_WU_MAIN_WALL_BUDGET_US,
            [this, mainBudgetWU](
                MSTaskLease& lease, MSWorkUnitCounter& work) {
                RunSchedulerTask(lease, work, mainBudgetWU);
            });
        cycleCompletedMarkWU_.fetch_add(
            slice.completedWU, std::memory_order_relaxed);

        uint64_t outstanding = markScheduler_.OutstandingHint();
        MSMarkWorkerControl::Snapshot control =
            markWorkerControl_.GetSnapshot();
        if (outstanding == 0 &&
            control.workersInFlight == 0 &&
            control.assistsInFlight == 0 &&
            markScheduler_.ReadyEmptyForTermination()) {
            cycleStats_.markTerminationReason =
                MSMarkTerminationReason::NATURAL;
            (void)QuiesceMarkWorkers();
            break;
        }

        size_t targetHelpers = ComputeMSMarkHelperTarget(
            outstanding,
            maxHelpers, pressure);
        if (targetHelpers != control.targetHelpers) {
            markWorkerControl_.SetTargetHelpers(targetHelpers);
        }

        if (markBudgetUs != 0 &&
            TimeUtil::MicroSeconds() - t0 >= markBudgetUs) {
            cycleStats_.markTerminationReason =
                MSMarkTerminationReason::TIME_BUDGET;
            (void)QuiesceMarkWorkers();
            break;
        }

        if (slice.completedTasks == 0) {
            (void)markWorkEpoch_.WaitForChange(
                observedProgressEpoch,
                std::chrono::microseconds(kMSMarkProgressWaitUs));
        }
    }

    cycleStats_.concurrentMarkUs = TimeUtil::MicroSeconds() - t0;
    VLOG(REPORT,
         "[MS-GC] ConcurrentMark scheduler: time=%sus termination=%s "
         "outstanding=%llu",
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         GetMSMarkTerminationReasonName(cycleStats_.markTerminationReason),
         static_cast<unsigned long long>(
             markScheduler_.OutstandingHint()));

    if (markPoolOwned) {
        collectorResources.ReleaseMSGCWorkerPool(
            MSGCWorkerPoolOwner::MARK);
    }
}


void MarkSweepCollector::StartMarkWorkers(size_t maxHelpers, size_t initialHelpers)
{
    markWorkerCount_ = maxHelpers;
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
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, i](size_t) { RunMarkWorker(i); }));
    }
    threadPool->Start();
}

void MarkSweepCollector::RunMarkWorker(size_t workerId)
{
    while (markWorkerControl_.WaitUntilEligible(workerId)) {
        if (!markWorkerControl_.TryEnterWorker(workerId)) {
            continue;
        }

        uint64_t observedProgressEpoch = markWorkEpoch_.Load();
        while (markWorkerControl_.CanContinueWorker(workerId)) {
            observedProgressEpoch = markWorkEpoch_.Load();
            uint64_t sliceBudget = ComputeMSMarkSliceBudget(
                markPacer_.GetThroughputQ16(),
                MS_WU_HELPER_WALL_BUDGET_US);
            MSMarkSliceResult slice = markScheduler_.RunMarkSlice(
                MSMarkWorkerCaps::GCWorker(), sliceBudget,
                MS_WU_HELPER_WALL_BUDGET_US,
                [this, sliceBudget](
                    MSTaskLease& lease, MSWorkUnitCounter& work) {
                    RunSchedulerTask(lease, work, sliceBudget);
                });
            if (slice.completedTasks == 0) {
                break;
            }
            cycleCompletedMarkWU_.fetch_add(
                slice.completedWU, std::memory_order_relaxed);
        }

        markWorkerControl_.ExitWorker();
        if (markWorkerControl_.CanContinueWorker(workerId)) {
            (void)markWorkEpoch_.WaitForChange(
                observedProgressEpoch, std::chrono::microseconds(kMSMarkProgressWaitUs));
        }
    }
}

size_t MarkSweepCollector::QuiesceMarkWorkers()
{
    markWorkerControl_.BeginQuiesce();
    markWorkerControl_.WaitForNoInFlight();
    size_t residualTasks = static_cast<size_t>(
        std::min<uint64_t>(
            markScheduler_.OutstandingHint(),
            std::numeric_limits<size_t>::max()));
    markWorkerControl_.Stop();
    if (markWorkerCount_ != 0) {
        GetThreadPool()->WaitFinish();
    }
    return residualTasks;
}


// ============================================================================
// MSMarkAssist: consume Region tasks in WorkUnit-bounded slices.
// ============================================================================
size_t MarkSweepCollector::MSMarkAssist(AllocBufferMS&, size_t debtWU)
{
    if (debtWU == 0) {
        return 0;
    }

    if (!markWorkerControl_.TryEnterAssist()) {
        return 0;
    }

    uint64_t budgetWU = std::max<uint64_t>(
        MS_WU_MIN_SLICE_BUDGET, debtWU);
    MSMarkSliceResult slice = markScheduler_.RunMarkSlice(
        MSMarkWorkerCaps::Assist(), budgetWU,
        MS_WU_ASSIST_WALL_BUDGET_US,
        [this, budgetWU](
            MSTaskLease& lease, MSWorkUnitCounter& work) {
            RunSchedulerTask(lease, work, budgetWU);
        });

    markWorkerControl_.ExitAssist();
    size_t paidWU = static_cast<size_t>(
        std::min<uint64_t>(
            slice.completedWU, std::numeric_limits<size_t>::max()));
    cycleCompletedMarkWU_.fetch_add(
        slice.completedWU, std::memory_order_relaxed);
    return std::min(paidWU, debtWU);
}

// ============================================================================
// Step 2.3: MarkTermination (STW)
// ============================================================================

void MarkSweepCollector::MarkTermination()
{
    uint64_t phaseStart = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- MarkTermination (STW) ---");
    VLOG(REPORT,
         "[MS-GC] [mark-stw-entry] branch=%s gcReason=%s outstanding=%llu",
         GetMSMarkTerminationReasonName(cycleStats_.markTerminationReason),
         g_gcRequests[gcReason].name,
         static_cast<unsigned long long>(
             markScheduler_.OutstandingHint()));

    // STW: freeze all mutators
    ScopedStopTheWorld stw("MS MarkTermination", true, GCPhase::GC_PHASE_MS_MARK_TERMINATION);

    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();

    // Mutators are frozen, so their owner-private entries are now stable.
    // Publish every residual old edge before closing the collector gate and
    // before evaluating the ordinary mark closure.
    if (centralPool != nullptr) {
        FlushAllBarrierMarkBuffersUnderSTW(*centralPool);
    }

    activeCollector_.store(nullptr, std::memory_order_release);

    // With no mutator or concurrent mark worker running, reconcile the
    // summary-first producer protocol to a true fixed point.  A producer may
    // have published a pending word while its Region was OWNED, observed that
    // state in TryPublishRegion(), and raced the owner's final post-release
    // check.  The Region itself is the durable record; this STW walk needs no
    // extra queue or producer-side control-word RMW.
    auto drainMarkClosureToFixedPoint = [this, centralPool](const char* closureName) {
        uint64_t closureWU = 0;
        size_t reconcilePasses = 0;
        size_t reconciledRegions = 0;
        size_t visitedRegions = 0;
        for (;;) {
            closureWU = SaturatingMSWUAdd(
                closureWU, DrainSchedulerUnderSTW());
            if (centralPool == nullptr) {
                break;
            }

            size_t passVisited = 0;
            size_t passReconciled = 0;
            centralPool->VisitMarkPendingRegionsUnderSTW(
                [this, &passVisited, &passReconciled](MSRegionInfo* region) {
                    CHECK_DETAIL(
                        region != nullptr &&
                        region->GetRegionKind() == MSRegionKind::SMALL &&
                        region->GetScanClass() == MSScanClass::HAS_REF,
                        "[MS-GC] invalid Region in pending-mark reconciliation");
                    ++passVisited;
                    if (region->LoadPendingMarkWordsAcquire() == 0) {
                        return;
                    }
                    CHECK_DETAIL(
                        region->GetScanState() == MSScanState::IDLE,
                        "[MS-GC] pending Region not IDLE after STW drain: idx=%zu state=%u",
                        MSRegionInfoArray::GetRegionIdx(region),
                        static_cast<unsigned>(region->GetScanState()));
                    ++passReconciled;
                    if (markScheduler_.TryPublishRegion(region)) {
                        markWorkEpoch_.Notify();
                    }
                });
            visitedRegions += passVisited;
            reconciledRegions += passReconciled;
            if (passReconciled == 0) {
                break;
            }
            ++reconcilePasses;
        }
        VLOG(REPORT,
             "[MS-GC] MarkTermination reconciliation: closure=%s passes=%zu "
             "regions=%zu visited=%zu",
             closureName, reconcilePasses, reconciledRegions, visitedRegions);
        return closureWU;
    };

    // 1. Complete the ordinary mark closure first. Finalizer reachability must
    // be classified against this stable bitmap, before resurrection marks are
    // added to it.
    uint64_t ordinaryTerminationWU = drainMarkClosureToFixedPoint("ordinary");
    VerifyMarkSchedulerQuiescence("ordinary");

    // 2. Snapshot the registered Finalizer population and move only objects
    // that are unreachable in the ordinary closure into the finalizable roots.
    PrepareFinalizersForResurrection();

    // 3. Mark newly-finalizable objects and their strong children to complete
    // the resurrection closure. Weak referents encountered here are registered
    // but are not followed as strong edges.
    MSMarkWordBatch resurrectionBatch;
    MSWorkUnitCounter resurrectionPublishWork;
    size_t resurrectionEntries = 0;
    VisitFinalizerRoots(
        [this, &resurrectionBatch, &resurrectionPublishWork,
         &resurrectionEntries](ObjectRef& ref) {
            ++resurrectionEntries;
            AddRefToMarkBatch(
                ref.object, false, resurrectionBatch,
                resurrectionPublishWork);
        });
    FlushMarkBatch(resurrectionBatch, resurrectionPublishWork);
    cycleCompletedMarkWU_.fetch_add(
        resurrectionPublishWork.Total(), std::memory_order_relaxed);
    uint64_t resurrectionClosureWU =
        drainMarkClosureToFixedPoint("resurrection");
    VerifyMarkSchedulerQuiescence("resurrection");

    // 4. The final mark bitmap is now stable. Resolve every live WeakRef before
    // mutators resume and before any Region can be published for sweep.
    ClearWeakRefsAfterMark();

    // 5. Diagnostic: marking is now complete and all mutators are frozen
    // (STW). This is the authoritative window to verify mark completeness —
    // no concurrent mutation can produce false positives. No-op unless the
    // env var cjMSVerifyMark is set.
    VerifyMarkCompleteness();

    cycleStats_.markTerminationUs = TimeUtil::MicroSeconds() - phaseStart;
    VLOG(REPORT,
         "[MS-GC] MarkTermination total: %sus ordinaryWU=%llu "
         "resurrectionRoots=%zu resurrectionPublishWU=%llu "
         "resurrectionClosureWU=%llu",
         Pretty(cycleStats_.markTerminationUs).Str(),
         static_cast<unsigned long long>(ordinaryTerminationWU),
         resurrectionEntries,
         static_cast<unsigned long long>(
             resurrectionPublishWork.Total()),
         static_cast<unsigned long long>(resurrectionClosureWU));

    // Restore IDLE INSIDE STW so that when Mutators resume, the mark barrier
    // has already been disabled. This eliminates a gap where a mutator could
    // publish new scheduler work after the last drain. The independent Sweep
    // epoch remains open and is tracked by CentralRegionPool counters.
    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, false);

    MSPostMarkSweepPlan sweepPlan = BuildMSPostMarkSweepPlan(gcReason == GC_REASON_OOM);
    if (centralPool != nullptr && sweepPlan.reenableAtMarkTermination) {
        centralPool->ReenableLazySweep();
    }
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

        // Partition the frozen LARGE collect-set only after statistics have
        // consumed it. Survivors are appended to largeNextSupply_; dead spans
        // are reset before publication to the large hot cache.
        CollectLargeGarbage();

        // The immutable Mark boundary becomes Sweep work only after the final
        // bitmap/stats consumers have finished. Regions returned or allocated
        // after PreEnum remain in swept allocation supply for the next cycle.
        centralPool->PublishCollectSetForSweep();

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
    size_t benignOutsideSetTargets = 0;
    size_t staleManagedTargets = 0;
    size_t staleNonTraceableTargets = 0;
    size_t staleInvalidTargets = 0;
    size_t staleReported = 0;
    const uint32_t currentMarkEpoch = centralPool->GetCurrentMarkEpoch();

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
            // TLS-active and post-boundary Regions have no current collectEpoch.
            // They are not reclaim candidates in this cycle.
            if (IsOutsideCurrentCollectSetObject(target, currentMarkEpoch)) {
                benignOutsideSetTargets++;
                return;
            }

            // Marked referrer -> unmarked current collect-set target.
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
         "benignOutsideSet=%zu, staleManaged=%zu, staleNonTraceable=%zu, staleInvalid=%zu",
         markedScanned, violations, benignOutsideSetTargets,
         staleManagedTargets, staleNonTraceableTargets, staleInvalidTargets);
}

// ============================================================================
// MarkNewObject: publish finalizer roots that cross a Mark phase boundary
// ============================================================================

void MarkSweepCollector::MarkNewObject(BaseObject* obj)
{
    GCPhase phase = GetGCPhase();
    if (LIKELY(phase != GCPhase::GC_PHASE_MS_MARK_SETUP) &&
        LIKELY(phase != GCPhase::GC_PHASE_MS_CONCURRENT_MARK) &&
        LIKELY(phase != GCPhase::GC_PHASE_MS_MARK_TERMINATION)) {
        return;
    }

    // Ordinary allocation does not call MarkNewObject. FinalizerWorkList invokes
    // it while holding listLock when a finalizer root is inserted or removed,
    // so the list mutation and scheduler publication form one linearization.
    MSMarkWordBatch batch;
    MSWorkUnitCounter work;
    AddRefToMarkBatch(obj, true, batch, work);
    FlushMarkBatch(batch, work);
    cycleCompletedMarkWU_.fetch_add(
        work.Total(), std::memory_order_relaxed);
}

// ============================================================================
// TracingCollector virtual overrides — simplified for Mark-Sweep (no tag/CAS)
// ============================================================================

void MarkSweepCollector::EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const
{
    BaseObject* target = MSLoadRawReference(ref);
    rootSet.push_back(target);
}

void MarkSweepCollector::EnumAndTagRawRoot(ObjectRef& root, RootSet& rootSet) const
{
    // MS version: no tag/CAS and no discovery during enumeration. The root
    // staging boundary flushes one coalescing batch after all sources merge.
    rootSet.push_back(root.object);
}

void MarkSweepCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack)
{
    auto refFunc = [this, &workStack](RefField<>& field) {
        BaseObject* target = MSLoadRawReference(field);
        TypeInfo* typeInfo = nullptr;
        if (DiscoverObject(target, &typeInfo) &&
            typeInfo != nullptr && typeInfo->HasRefField()) {
            workStack.push_back(target);
        }
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
    uint32_t sweepCycleId = CentralRegionPool::GetSweepCycleId();
    VLOG(REPORT, "[MS-GC] === GC START cycle=%llu === sweepCycleId=%u reason=%u(%s)",
         static_cast<unsigned long long>(gcCycle_), sweepCycleId,
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
    uint32_t sweepCycleId = CentralRegionPool::GetSweepCycleId();
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);

    VLOG(REPORT, "[MS-GC] --- Total ---");
    VLOG(REPORT, "[MS-GC] region statistics when gc ends (sweepCycleId=%u)", sweepCycleId);

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
        size_t clsTotal = pressure.partialSwept + pressure.fullSwept + pressure.unswept;
        if (clsTotal == 0) {
            continue;
        }
        VLOG(REPORT,
             "[MS-GC]     cls=%u partialSwept=%zu fullSwept=%zu unswept=%zu total=%zu",
             static_cast<unsigned>(cls),
             pressure.partialSwept, pressure.fullSwept,
             pressure.unswept, clsTotal);
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
    // A multiplicative target alone gives a tiny allocation runway when the
    // stable live set is small. Region marking then repeatedly enters MARK and
    // assist before the previous cycle's fixed costs can be amortized. Keep
    // five allocator segments of runway while preserving the configured
    // multiplier for large live sets. The heap cap remains a hard bound.
    constexpr size_t kMSMinConcurrentMarkRunwayBytes =
        static_cast<size_t>(5) * kMSShardSegmentBytes;
    size_t absoluteMinThreshold =
        std::min(gcThresholdCap, static_cast<size_t>(20) * 1024 * 1024);
    size_t minThreshold = ComputeMSHeapThresholdFloor(
        baseBytes, absoluteMinThreshold, kMSMinConcurrentMarkRunwayBytes,
        gcThresholdCap);
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
    uint64_t completedWU =
        cycleCompletedMarkWU_.load(std::memory_order_relaxed);
    uint64_t allocatedMarkBytes =
        cycleAllocatedMarkBytes_.load(std::memory_order_relaxed);
    markPacer_.UpdateFullCycle(
        completedWU, allocatedMarkBytes, cycleStats_.concurrentMarkUs);
    VLOG(REPORT,
         "[MS-GC] mark pacer: completedWU=%llu allocatedBytes=%llu "
         "concurrentUs=%llu densityQ16=%llu throughputQ16=%llu",
         static_cast<unsigned long long>(completedWU),
         static_cast<unsigned long long>(allocatedMarkBytes),
         static_cast<unsigned long long>(cycleStats_.concurrentMarkUs),
         static_cast<unsigned long long>(markPacer_.GetWorkDensityQ16()),
         static_cast<unsigned long long>(markPacer_.GetThroughputQ16()));
    VLOG(REPORT,
         "[MS-GC] heapThreshold: oldThreshold=%zuB newThreshold=%zuB "
         "oldScavengeThreshold=%zuB newScavengeThreshold=%zuB "
         "prevLive=%zuB prevStableLive=%zuB trueLive=%zuB base=%zuB "
         "markStartInUse=%zuB markEndInUse=%zuB inUseBurst=%zuB "
         "floatingLiveInflated=%d acceptedSample=%d historySize=%zu "
         "medianStableLive=%zuB exceptionalBypass=%d "
         "growth=%.2f minRunway=%zuB minThreshold=%zuB "
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
         1.0 + heapGrowth, kMSMinConcurrentMarkRunwayBytes, minThreshold,
         thresholdDecision.rawTarget,
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
