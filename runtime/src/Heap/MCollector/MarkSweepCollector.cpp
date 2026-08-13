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
#include <vector>
#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/SysCall.h"
#include "Common/BaseObject.h"
#include "CangjieRuntime.h"
#include "Common/GCRefFieldScan.inline.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSParallelSweepPolicy.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSRegionManager.h"
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Heap/MAllocator/MSShardRouting.h"
#include "Heap/MBarrier/MSMarkBarrier.h"
#include "Heap/MBarrier/MSRefField.inline.h"
#include "Heap/MCollector/MSHeapThresholdPolicy.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Collector/ForwardDataManager.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/RefField.inline.h"
#include "TypeInfoManager.h"
#include "schedule.h"

#ifndef _WIN32
#include <unistd.h>  // usleep
#endif

namespace MapleRuntime {

namespace {
constexpr uint64_t kMSMarkProgressWaitUs = 50;
constexpr uint64_t kMSTerminationDrainWaitUs = 5;
constexpr size_t kMSInvalidMarkReadyParticipant =
    std::numeric_limits<size_t>::max();
thread_local size_t g_msMarkReadyParticipant =
    kMSInvalidMarkReadyParticipant;

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

bool HasReachedMarkSchedulerQuiescence(
    uint64_t outstanding, const MSMarkWorkerControl::Snapshot& control,
    bool readyEmpty)
{
    return outstanding == 0 && control.workersInFlight == 0 &&
           control.assistsInFlight == 0 && readyEmpty;
}

bool HasReachedMarkTimeBudget(
    uint64_t elapsedMarkUs, uint64_t markBudgetUs)
{
    return markBudgetUs != 0 && elapsedMarkUs >= markBudgetUs;
}

}

static bool IsVerifyMarkEnabled()
{
    return CangjieRuntime::GetGCParam().msVerifyMark != 0;
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
    uint32_t regionIdx = MSRegionInfo::INVALID_REGION_IDX;
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

template<bool Trusted>
struct MarkSourceTag {};

static inline bool ValidatePublishedMarkTarget(BaseObject* obj, MarkSourceTag<true>)
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    CHECK_DETAIL(obj->GetTypeInfo() != nullptr,
                 "[MS-GC] trusted mark target is not published obj=%p",
                 static_cast<void*>(obj));
#else
    (void)obj;
#endif
    return true;
}

static inline bool ValidatePublishedMarkTarget(BaseObject* obj, MarkSourceTag<false>)
{
    return obj->GetTypeInfo() != nullptr;
}

static inline bool ResolveNonSmallMarkTarget(
    uintptr_t addr, MSRegionInfo* info, ResolvedMarkTarget& out)
{
    if (!info->IsLargeRegionEntry()) {
        return false;
    }
    MSRegionInfo* base = MSRegionInfoArray::ResolveLargeBaseInfo(info);
    if (base == nullptr || base->allocPtr != addr || base->regionEnd < addr ||
        base->regionEnd - addr < sizeof(uintptr_t)) {
        return false;
    }
    out.info = base;
    out.regionIdx = static_cast<uint32_t>(MSRegionInfoArray::GetRegionIdx(base));
    out.isLarge = true;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    CHECK_DETAIL(MSRegionInfoArray::GetRegionInfoValidated(addr) == info,
                 "[MS-GC] direct LARGE Region lookup disagrees with validated lookup: "
                 "addr=%p direct=%p",
                 reinterpret_cast<void*>(addr), static_cast<void*>(info));
#endif
    return true;
}

static inline bool ResolveDefensiveMarkTarget(
    BaseObject* obj, ResolvedMarkTarget& out)
{
    out = ResolvedMarkTarget {};
    if (obj == nullptr) {
        return false;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (!IsManagedAddress(addr)) {
        return false;
    }

    size_t regionOffset = 0;
    size_t regionIdx = 0;
    MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoDirect(
        addr, regionOffset, regionIdx);
    if (LIKELY(info->GetRegionKind() == MSRegionKind::SMALL)) {
        uint8_t sizeClass = info->sizeClass;
        if (UNLIKELY(sizeClass >= kNumSizeClasses)) {
            return false;
        }
        const MSSizeClassDesc& desc = SizeClassInfo::ClassToDesc(sizeClass);
        size_t slotIdx = SizeClassInfo::OffsetToSlotIndex(sizeClass, regionOffset);
        if (UNLIKELY(slotIdx >= desc.totalSlots ||
                     slotIdx * desc.slotSize != regionOffset)) {
            return false;
        }
        out.info = info;
        out.regionIdx = static_cast<uint32_t>(regionIdx);
        out.wordIdx = slotIdx / 64;
        out.bitMask = UINT64_C(1) << (slotIdx % 64);
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        CHECK_DETAIL(MSRegionInfoArray::GetRegionInfoValidated(addr) == info,
                     "[MS-GC] defensive direct Small lookup disagrees with validation obj=%p",
                     static_cast<void*>(obj));
#endif
        return true;
    }
    return ResolveNonSmallMarkTarget(addr, info, out);
}

void MarkSweepCollector::InitializeGCMarkBatch(MSMarkWordBatch& batch) const
{
    uint32_t markEpoch = __atomic_load_n(
        &CJ_MRT_MSGCFastPathState.currentMarkEpoch, __ATOMIC_ACQUIRE);
    uintptr_t allocPlaneBase = static_cast<uintptr_t>(__atomic_load_n(
        &CJ_MRT_MSGCFastPathState.allocPlaneBase, __ATOMIC_ACQUIRE));
    uintptr_t markPlaneBase = static_cast<uintptr_t>(__atomic_load_n(
        &CJ_MRT_MSGCFastPathState.markPlaneBase, __ATOMIC_ACQUIRE));
    CHECK_DETAIL(
        markEpoch != MSRegionInfo::NO_COLLECT_EPOCH &&
        allocPlaneBase != 0 && markPlaneBase != 0,
        "[MS-GC] GC MarkWordBatch initialized without a cycle bitmap context");
    batch.SetMarkEpoch(markEpoch);
    batch.SetBitmapPlaneBases(allocPlaneBase, markPlaneBase);
}

// Runtime slow-path view over the compiler-visible mutator buffer. The POD
// storage and its lookup policy remain ABI-stable, while object validation,
// bitmap marking and scheduler publication use the same collector templates as
// GC-owned batches.
class MSBarrierMarkBatchStorageView {
public:
    MSBarrierMarkBatchStorageView(MSBarrierMarkBuffer& buffer,
                                  uint32_t markEpoch,
                                  uintptr_t allocPlaneBase,
                                  uintptr_t markPlaneBase)
        : buffer_(buffer), markEpoch_(markEpoch),
          allocPlaneBase_(allocPlaneBase), markPlaneBase_(markPlaneBase)
    {
        CHECK_DETAIL(allocPlaneBase_ != 0 && markPlaneBase_ != 0,
                     "[MS-GC] barrier batch without published bitmap planes");
    }

    bool HasMarkEpoch() const { return true; }
    uint32_t GetMarkEpoch() const { return markEpoch_; }
    bool HasBitmapPlaneBases() const { return true; }

    void SetMarkEpoch(uint32_t markEpoch)
    {
        CHECK_DETAIL(markEpoch_ == markEpoch,
                     "[MS-GC] barrier batch epoch changed old=%u new=%u",
                     markEpoch_, markEpoch);
    }

    void SetBitmapPlaneBases(uintptr_t allocPlaneBase,
                             uintptr_t markPlaneBase)
    {
        CHECK_DETAIL(
            allocPlaneBase_ == allocPlaneBase &&
            markPlaneBase_ == markPlaneBase,
            "[MS-GC] barrier batch bitmap planes changed");
    }

    uint64_t* GetAllocBitmap(uint32_t regionIdx) const
    {
        return reinterpret_cast<uint64_t*>(
            allocPlaneBase_ + static_cast<uintptr_t>(regionIdx) *
                MSBitmapArena::kPlaneStrideBytes);
    }

    uint64_t* GetMarkBitmap(uint32_t regionIdx) const
    {
        return reinterpret_cast<uint64_t*>(
            markPlaneBase_ + static_cast<uintptr_t>(regionIdx) *
                MSBitmapArena::kPlaneStrideBytes);
    }

    template<typename FlushEntry>
    void Add(uint32_t regionIdx, size_t wordIdx, uint64_t mask,
             FlushEntry&& flushEntry)
    {
        if (UNLIKELY(
                buffer_.pendingBitCount >=
                kMSBarrierMarkBufferPendingBitFlushThreshold)) {
            FlushAll(flushEntry);
        }

        uint64_t wordKey = MakeMSBarrierMarkWordKey(
            regionIdx, static_cast<uint32_t>(wordIdx));
        MSBarrierMarkLookup lookup =
            LookupMSBarrierMarkEntry(buffer_, wordKey);
        uint8_t existing = lookup.existingEntry;
        if (existing != kMSBarrierMarkBufferInvalidEntry &&
            (buffer_.entries[existing].mask & mask) == mask) {
            buffer_.lastEntry = existing;
            return;
        }

        MSBarrierMarkAddResult result =
            TryAddMSBarrierMarkSingleBitAtLookup(
                buffer_, wordKey, mask, lookup);
        if (UNLIKELY(result == MSBarrierMarkAddResult::NEEDS_FLUSH)) {
            FlushAll(flushEntry);
            result = TryAddMSBarrierMarkSingleBitAtLookup(
                buffer_, wordKey, mask,
                LookupMSBarrierMarkEntry(buffer_, wordKey));
            CHECK_DETAIL(
                result != MSBarrierMarkAddResult::NEEDS_FLUSH,
                "[MS-GC] empty barrier mark buffer rejected one bit key=0x%llx",
                static_cast<unsigned long long>(wordKey));
        }

    }

    template<typename FlushEntry>
    void FlushAll(FlushEntry&& flushEntry)
    {
        uint32_t occupied = buffer_.occupancy;
        while (occupied != 0) {
            uint32_t entryIdx =
                static_cast<uint32_t>(__builtin_ctz(occupied));
            occupied &= occupied - 1;
            const MSBarrierMarkEntry& source = buffer_.entries[entryIdx];
            MSMarkWordBatchEntry entry;
            entry.wordKey = source.wordKey;
            entry.mask = source.mask;
            flushEntry(entry);
        }
        ClearMSBarrierMarkPendingBits(buffer_);
    }

    bool IsEmpty() const { return IsMSBarrierMarkBufferEmpty(buffer_); }

private:
    MSBarrierMarkBuffer& buffer_;
    uint32_t markEpoch_;
    uintptr_t allocPlaneBase_;
    uintptr_t markPlaneBase_;
};

// Static member definition
std::atomic<MarkSweepCollector*> MarkSweepCollector::activeCollector_{nullptr};

// ============================================================================
// Lifecycle
// ============================================================================

void MarkSweepCollector::Init()
{
    LOG(RTLOG_REPORT, "[MS-GC] MarkSweepCollector::Init()");
    TracingCollector::Init();
    markReadyProcessorCount_ = Runtime::CurrentRef() != nullptr
        ? static_cast<size_t>(
            Runtime::Current().GetConcurrencyModel().GetProcessorNum())
        : 1;
    size_t totalGCMarkThreads = std::min(
        static_cast<size_t>(std::max<int32_t>(
            1, CangjieRuntime::GetGCParam().gcThreads)),
        CangjieRuntime::GetMSGCThreadCapacity());
    size_t markReadyShardCount = ComputeMSMarkReadyShardCount(
        markReadyProcessorCount_, totalGCMarkThreads);
    markScheduler_.ConfigureRegionShards(markReadyShardCount);
    UpdateMarkAssistPlan();
    VLOG(REPORT,
         "[MS-GC] Mark ready shards: P=%zu gcThreads=%zu shards=%zu",
         markReadyProcessorCount_, totalGCMarkThreads,
         markScheduler_.RegionShardCount());
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
    // MarkTermination has already restored the idle barrier before Mutators
    // resume.  The Sweep epoch is tracked independently by CentralRegionPool.
    collectorResources.NotifyGCFinished(gcIndex);

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    DumpAfterGC();
#endif
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
    currentGcStartUs_ = TimeUtil::MicroSeconds();
    LogGCEntry(gcIndex, reason);
    PreGarbageCollection(/* isConcurrent */ true);
    DumpGCStateSnapshot("preGCMark");
    DoGarbageCollection();
    PostGarbageCollection(gcIndex);

    gcStats.gcEndTime = TimeUtil::NanoSeconds();
    uint64_t gcTimeNs = gcStats.gcEndTime - gcStats.gcStartTime;
    uint64_t gcTimeUs = gcTimeNs / NS_PER_US;

    DumpRegionStatsMS();
    LogGCSummary(gcTimeUs);

    g_gcCount++;
    g_gcTotalTimeUs += gcTimeUs;
    g_gcCollectedTotalBytes += gcStats.collectedBytes;
}

void MarkSweepCollector::DoGarbageCollection()
{
    if (gcReason == GC_REASON_OOM) {
        RunOOMEmergencyCycle();
        return;
    }
    PreEnumRoots();        // Step 2.1a
    EnumRoots();           // Step 2.1b
    ConcurrentMark();      // Step 2.2
    MarkTermination();     // Step 2.3
    PostMarkTermination(); // Step 2.4
}

void MarkSweepCollector::RunOOMEmergencyCycle()
{
    // An OOM notification takes one uninterrupted STW window. In particular,
    // no mutator may allocate between the frozen mark preparation, root scan,
    // mark closure, and the PostMarkTermination handoff to normal Sweep.
    // Do not install MS_MARK_SETUP in the outer STW: PreEnumRoots must clear
    // the prior bitmap epoch before EnumRoots asks mutators to mark roots.
    ScopedStopTheWorld stw(
        "MS OOM Mark through PostMarkTermination");
    PreEnumRoots(true);
    EnumRoots();
    ConcurrentMark();
    MarkTermination(true);
    PostMarkTermination();
}

// ============================================================================
// Step 2.1a: PreEnumRoots  (GC thread only)
//   Stop all sweeps, wait for in-flight background/assist/helper work to quiesce, clear
//   markBitmaps, then briefly stop the world for reclassify + generation flip
//   that freezes this cycle's sweep snapshot.
//
// ============================================================================

void MarkSweepCollector::PreEnumRoots(bool worldStopped)
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    cycleCompletedMarkWU_.store(0, std::memory_order_relaxed);
    assistWorkPerByteScaled_.store(0, std::memory_order_relaxed);
    markAssistCompletedWU_.store(0, std::memory_order_relaxed);
    // GC main follows the P participants in the stable affinity namespace.
    // The hint remains valid through root publication, concurrent Mark and
    // both MarkTermination closure drains on this GC thread.
    g_msMarkReadyParticipant = markReadyProcessorCount_;


    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    // Capture the capacity used during the preceding allocation period
    // before its dead Regions become empty/hot-free. Scavenge uses this
    // as a cache floor after the Sweep epoch is forced to completion.
    cycleStats_.preSweepInUseBytes = space.GetHeapInUseBytes();
    
    // Stop sweep → wait for in-flight sweep to quiesce → under STW clear
    // bitmaps / reclassify residual unswept entries / flip sweep generation
    // BEFORE concurrent mark begins. Flipping the generation here (rather than
    // in MarkTermination) guarantees that every region allocated during this
    // GC cycle's mark phase carries the NEW sweep generation, so the current
    // sweep skips them. Otherwise such regions keep the OLD generation and get
    // swept this cycle, which wrongly reclaims live objects that are only held
    // by stack/local roots (those never pass through the write barrier).
    // Safe pre-mark: no SweepRegion running, mark-at-alloc inactive.
    CentralRegionPool* centralPool = space.GetCentralPool();
    CHECK_DETAIL(centralPool != nullptr,
                 "[MS-GC] RegionSpaceMS has no CentralRegionPool in PreEnumRoots");
    {
        centralPool->StopAndWaitForAllSweep();
        auto prepareUnderSTW = [&]() {
            markScheduler_.ResetCycle();
            centralPool->PrepareMarkEpochUnderSTW(gcReason == GC_REASON_OOM);

            // Keep the old collector gate closed until every prior-epoch
            // scheduler and barrier-buffer state has been discarded.  In
            // particular, an AllocBufferMS that exits while sweep is being
            // stopped must reset a stale buffer instead of flushing it into
            // the old TaskLedger just before ResetCycle().  Mutators remain
            // stopped here, so opening the gate after the new epoch is fully
            // prepared cannot race a barrier publication.
            activeCollector_.store(this, std::memory_order_release);
        };
        if (worldStopped) {
            prepareUnderSTW();
        } else {
            ScopedStopTheWorld stw("MS PreEnumRoots clear/reclassify/prepare STW");
            prepareUnderSTW();
        }
    }
    cycleStats_.preEnumRootsUs = TimeUtil::MicroSeconds() - t0;
}

// ============================================================================
// Step 2.1b: EnumRoots  (handshake + root enumeration)
//   Phase B (handshake): TransitionToGCPhase(MS_MARK_SETUP) — each Mutator
//     scans its own stack roots at safepoint.
//   Phase C (GC thread): EnumAndMarkRoots — global roots + mark root set.
// ============================================================================

void MarkSweepCollector::EnumRoots()
{
    uint64_t startUs = TimeUtil::MicroSeconds();
    // Handshake: each Mutator reaches safepoint, scans its own stack roots.
    MutatorManager::Instance().TransitionAllMutatorsToGCPhase(
        GCPhase::GC_PHASE_MS_MARK_SETUP);

    // GC thread enumerates global roots + marks root set.
    rootSnapshot_.Clear();
    EnumAndMarkRoots();
    PublishRootStaging();
    cycleStats_.enumRootsUs = TimeUtil::MicroSeconds() - startUs;
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
    if (UNLIKELY(!ResolveDefensiveMarkTarget(obj, target))) {
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

template<typename MarkBatch, typename WorkCounter>
void MarkSweepCollector::AddMarkBitToBatch(
    uint32_t regionIdx, size_t wordIdx, uint64_t bitMask,
    MarkBatch& batch, WorkCounter& work,
    MSMarkPublicationContext& publication)
{
    batch.Add(regionIdx, wordIdx, bitMask,
              [this, &work, &batch,
               &publication](const MSMarkWordBatchEntry& entry) {
                  MSRegionInfo* publishInfo =
                      FlushMarkWordEntryImpl(entry, work, batch);
                  if (publishInfo == nullptr) {
                      return;
                  }
                  StageMarkPublication(
                      publishInfo, entry.WordIndex(), publication);
              });
}

template<bool Trusted, typename MarkBatch, typename WorkCounter>
void MarkSweepCollector::AddResolvedRefToMarkBatchImpl(
    BaseObject* obj, const ResolvedMarkTarget& target, MarkBatch& batch,
    WorkCounter& work, MSMarkPublicationContext& publication)
{
    // LARGE/SPAN has one canonical mark flag and one OBJECT task. It is not
    // mixed into the small-object word batch.
    if (target.isLarge) {
        if (LIKELY(target.IsMarked())) {
            return;
        }
        if (UNLIKELY(!ValidatePublishedMarkTarget(
                obj, MarkSourceTag<Trusted> {}))) {
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

    uint32_t currentEpoch = batch.GetMarkEpoch();
    uint32_t regionIdx = target.regionIdx;
    if (!Trusted) {
        uint64_t* markWord =
            &batch.GetMarkBitmap(regionIdx)[target.wordIdx];
        // Defensive sources retain the shared/cyclic-graph prefilter.
        // Trusted barriers and roots avoid this per-ref shared bitmap load and
        // let the coalesced flush perform it once per bitmap word.
        if (LIKELY((__atomic_load_n(markWord, __ATOMIC_RELAXED) &
                    target.bitMask) != 0)) {
            return;
        }

        bool inCollectSet = target.info->collectEpoch == currentEpoch;
        if (!inCollectSet) {
            // An untrusted Runtime value can name an aligned stale slot.
            // OUTSIDE_SET allocation state is mutable, so retain stateWord
            // publication validation only for this defensive source.
            if (UNLIKELY(!ValidatePublishedMarkTarget(
                    obj, MarkSourceTag<Trusted> {}))) {
                return;
            }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
            if (target.info->GetScanClass() == MSScanClass::NO_REF) {
                TypeInfo* typeInfo = obj->GetTypeInfo();
                CHECK_DETAIL(!typeInfo->HasRefField(),
                             "[MS-GC] NO_REF Region contains HAS_REF object obj=%p idx=%u",
                             static_cast<void*>(obj), target.regionIdx);
            }
#endif
        }
    }

    AddMarkBitToBatch(
        regionIdx, target.wordIdx, target.bitMask,
        batch, work, publication);
}

template<MarkSweepCollector::MSMarkRefSource Source, typename MarkBatch,
         typename WorkCounter>
void MarkSweepCollector::AddRefToMarkBatch(
    BaseObject* obj, MarkBatch& batch, WorkCounter& work,
    MSMarkPublicationContext& publication)
{
    if (Source == MSMarkRefSource::TRUSTED_MAYBE_MANAGED) {
        if (UNLIKELY(!Heap::IsHeapAddress(obj))) {
            return;
        }
        (void)ValidatePublishedMarkTarget(obj, MarkSourceTag<true> {});
    }

    if (Source == MSMarkRefSource::DEFENSIVE) {
        ResolvedMarkTarget target;
        if (UNLIKELY(!ResolveDefensiveMarkTarget(obj, target))) {
            return;
        }
        work.AddResolvedRefs();
        AddResolvedRefToMarkBatchImpl<false>(
            obj, target, batch, work, publication);
        return;
    }

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    CHECK_DETAIL(obj != nullptr && Heap::IsHeapAddress(obj),
                 "[MS-GC] trusted mark target is not a managed ref obj=%p",
                 static_cast<void*>(obj));
#endif
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    size_t regionOffset = 0;
    size_t resolvedRegionIdx = 0;
    MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoDirect(
        addr, regionOffset, resolvedRegionIdx);
    if (LIKELY(info->GetRegionKind() == MSRegionKind::SMALL)) {
        uint8_t sizeClass = info->sizeClass;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        CHECK_DETAIL(sizeClass < kNumSizeClasses,
                     "[MS-GC] trusted Small Region has invalid sizeClass=%u",
                     static_cast<unsigned>(sizeClass));
#endif
        size_t slotIdx =
            SizeClassInfo::OffsetToSlotIndex(sizeClass, regionOffset);
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        size_t slotSize = info->GetSlotSize();
        CHECK_DETAIL(info->GetTotalSlotCount() > 0 && slotSize != 0 &&
                         slotIdx < info->GetTotalSlotCount() &&
                         slotIdx * slotSize == regionOffset,
                     "[MS-GC] trusted ref is not an exact Small slot obj=%p region=%zu",
                     static_cast<void*>(obj), resolvedRegionIdx);
        CHECK_DETAIL(MSRegionInfoArray::GetRegionInfoValidated(addr) == info,
                     "[MS-GC] trusted direct Small lookup disagrees with validation obj=%p",
                     static_cast<void*>(obj));
#endif
        work.AddResolvedRefs();
        AddMarkBitToBatch(
            static_cast<uint32_t>(resolvedRegionIdx), slotIdx / 64,
            UINT64_C(1) << (slotIdx % 64), batch, work, publication);
        return;
    }

    ResolvedMarkTarget target;
    if (UNLIKELY(!ResolveNonSmallMarkTarget(addr, info, target))) {
        return;
    }
    work.AddResolvedRefs();
    AddResolvedRefToMarkBatchImpl<true>(
        obj, target, batch, work, publication);
}

template<typename MarkBatch, typename WorkCounter>
MSRegionInfo* MarkSweepCollector::FlushMarkWordEntryImpl(
    const MSMarkWordBatchEntry& entry, WorkCounter& work,
    const MarkBatch& batch)
{
    uint32_t regionIdx = entry.RegionIndex();
    uint8_t wordIdx = entry.WordIndex();
    MSRegionInfo* info =
        MSRegionInfoArray::GetRegionInfoByIdx(regionIdx);
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    CHECK_DETAIL(
        info->GetRegionKind() == MSRegionKind::SMALL &&
        wordIdx < info->GetBitmapWordCount(),
        "[MS-GC] stale MarkWordBatch entry region=%u word=%u kind=%u words=%u",
        regionIdx, static_cast<unsigned>(wordIdx),
        static_cast<unsigned>(info->GetRegionKind()),
        static_cast<unsigned>(info->GetBitmapWordCount()));
#endif

    uint64_t mask = entry.mask;
    uint32_t markEpoch = batch.GetMarkEpoch();
    if (info->collectEpoch == markEpoch) {
        uint64_t validMask =
            wordIdx + 1 == info->GetBitmapWordCount()
            ? info->GetValidTailMask()
            : ~UINT64_C(0);
        uint64_t allocWord =
            __atomic_load_n(&batch.GetAllocBitmap(regionIdx)[wordIdx],
                            __ATOMIC_RELAXED);
        mask &= ~allocWord & validMask;
    }
    if (mask == 0) {
        return nullptr;
    }

    uint64_t* markWord = &batch.GetMarkBitmap(regionIdx)[wordIdx];
    mask &= ~__atomic_load_n(markWord, __ATOMIC_RELAXED);
    if (mask == 0) {
        return nullptr;
    }

    work.AddMarkWordRMW();
    uint64_t old = __atomic_fetch_or(markWord, mask, __ATOMIC_RELAXED);
    uint64_t newBits = mask & ~old;
    if (newBits == 0 || info->GetScanClass() == MSScanClass::NO_REF) {
        return nullptr;
    }
    return info;
}

template<size_t Capacity>
bool MarkSweepCollector::PublishPendingMarkRegions(
    MSPendingMarkRegionBatch<Capacity>& pending)
{
    bool schedulerPublished = false;
    size_t preferredRegionShard = GetCurrentMarkReadyShard();
    pending.FlushAll([this, &schedulerPublished, preferredRegionShard](
        MSRegionInfo* info, uint64_t wordMask) {
        uint64_t newWords = markScheduler_.PublishRegionWords(info, wordMask);
        if (newWords != 0) {
            schedulerPublished |= markScheduler_.TryPublishRegion(
                info, preferredRegionShard);
        }
    });
    return schedulerPublished;
}

void MarkSweepCollector::FlushMarkPublication(
    MSMarkPublicationContext& publication, bool notifyReadyWorkers)
{
    publication.readyPublished |=
        PublishPendingMarkRegions(publication.pending);
    if (notifyReadyWorkers && publication.readyPublished) {
        markWorkEpoch_.Notify();
        publication.readyPublished = false;
    }
}

void MarkSweepCollector::StageMarkPublication(
    MSRegionInfo* info, size_t wordIdx,
    MSMarkPublicationContext& publication)
{
    if (LIKELY(publication.pending.TryAdd(info, wordIdx))) {
        return;
    }
    // Bound publication latency for a high-fanout object. The active parent
    // lease protects termination while this small task-local batch coalesces.
    FlushMarkPublication(publication, true);
    CHECK_DETAIL(
        publication.pending.TryAdd(info, wordIdx),
        "[MS-GC] empty pending Region batch rejected publication");
}

template<typename MarkBatch, typename WorkCounter>
void MarkSweepCollector::FlushMarkBatch(
    MarkBatch& batch, WorkCounter& work,
    MSMarkPublicationContext& publication)
{
    if (!batch.IsEmpty()) {
        batch.FlushAll([this, &work, &batch,
                        &publication](const MSMarkWordBatchEntry& entry) {
            MSRegionInfo* publishInfo =
                FlushMarkWordEntryImpl(entry, work, batch);
            if (publishInfo != nullptr) {
                StageMarkPublication(
                    publishInfo, entry.WordIndex(), publication);
            }
        });
    }
    FlushMarkPublication(publication, true);
}

void MarkSweepCollector::AddManagedRefToBarrierBuffer(
    BaseObject* ref, MSBarrierMarkBuffer& buffer,
    uint32_t currentMarkEpoch, uintptr_t allocPlaneBase,
    uintptr_t markPlaneBase)
{
    MSBarrierMarkBatchStorageView batch(
        buffer, currentMarkEpoch, allocPlaneBase, markPlaneBase);
    MSNoopWorkUnitCounter noWork;
    MSMarkPublicationContext publication;
    AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MANAGED>(
        ref, batch, noWork, publication);
    FlushMarkPublication(publication, true);
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

    MSBarrierMarkBatchStorageView batch(
        buffer, currentEpoch, allocPlaneBase, markPlaneBase);
    MSNoopWorkUnitCounter noWork;
    MSMarkPublicationContext publication;
    FlushMarkBatch(batch, noWork, publication);
}

void MarkSweepCollector::NotifyAllocBufferExit(AllocBufferMS& allocBuffer)
{
    FoldGCMarkAssistWork(allocBuffer);
    FlushBarrierMarkBuffer(allocBuffer.GetBarrierMarkBuffer());
}

void MarkSweepCollector::FoldGCMarkAssistWork(
    AllocBufferMS& allocBuffer)
{
    uint64_t completedWU = allocBuffer.DrainGCMarkAssistWork();
    if (completedWU != 0) {
        markAssistCompletedWU_.fetch_add(
            completedWU, std::memory_order_relaxed);
    }
}

void MarkSweepCollector::FoldActiveGCMarkAssistWork()
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool == nullptr) {
        return;
    }
    centralPool->VisitAllocBufferMS([this](AllocBufferMS& buffer) {
        FoldGCMarkAssistWork(buffer);
    });
}

void MarkSweepCollector::FlushAllBarrierMarkBuffersUnderSTW(CentralRegionPool& central)
{
    central.VisitAllocBufferMS([this](AllocBufferMS& buffer) {
        MSBarrierMarkBuffer& markBuffer = buffer.GetBarrierMarkBuffer();
        if (markBuffer.occupancy == 0) {
            return;
        }
        FlushBarrierMarkBuffer(markBuffer);
    });
}

void MarkSweepCollector::RunRegionTask(
    MSTaskLease& lease, MSWorkUnitCounter& work)
{
    MSRegionInfo* info = lease.GetRegionInfo();
    CHECK_DETAIL(
        info != nullptr &&
        info->GetRegionKind() == MSRegionKind::SMALL &&
        info->GetScanClass() == MSScanClass::HAS_REF &&
        info->GetScanState() == MSScanState::OWNED,
        "[MS-GC] invalid Region task lease idx=%u", lease.GetRegionIdx());

    MSMarkWordBatch batch;
    InitializeGCMarkBatch(batch);
    MSMarkPublicationContext& publication = batch.Publication();
    uint32_t currentEpoch = batch.GetMarkEpoch();
    bool inCollectSet = info->collectEpoch == currentEpoch;
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
    uint64_t claimedSummary = info->TakePendingMarkWords();
    uint64_t summary = claimedSummary;
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

            if (UNLIKELY(typeInfo->IsWeakRefType())) {
                weakRefs.Add(obj);
            } else if (LIKELY(typeInfo->HasRefField())) {
                // HAS_REF is a Region allocation class, not a per-object
                // guarantee. Finalizable and conservatively classified
                // objects may legitimately have no reference fields.
                auto refVisitor =
                    [this, &batch, &work,
                     &publication](RefField<>& field) {
                        AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MAYBE_MANAGED>(
                            MSLoadRawReference(field), batch, work,
                            publication);
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
    }

    FlushMarkBatch(batch, work, publication);
    weakRefs.Flush();
    markScheduler_.CompleteRegionWork(info, claimedSummary);

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
        markScheduler_.TryPublishRegion(
            info, GetCurrentMarkReadyShard())) {
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
    InitializeGCMarkBatch(batch);
    MSMarkPublicationContext& publication = batch.Publication();
    MSWeakRefLocalBatch weakRefs;
    if (UNLIKELY(typeInfo->IsWeakRefType())) {
        weakRefs.Add(obj);
    } else {
        auto refVisitor = [this, &batch, &work,
                           &publication](RefField<>& field) {
            AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MAYBE_MANAGED>(
                MSLoadRawReference(field), batch, work, publication);
        };
        ForEachRefFieldGCKnownType(obj, typeInfo, refVisitor);
    }
    FlushMarkBatch(batch, work, publication);
    weakRefs.Flush();
    markScheduler_.CompleteObjectWork(info);
    lease.Complete(batch.IsEmpty());
}

void MarkSweepCollector::RunSchedulerTask(
    MSTaskLease& lease, MSWorkUnitCounter& work)
{
    if (lease.GetKind() == MSMarkTaskKind::REGION) {
        RunRegionTask(lease, work);
    } else {
        RunObjectTask(lease, work);
    }
}

size_t MarkSweepCollector::GetCurrentMarkReadyShard() const
{
    size_t participant = g_msMarkReadyParticipant;
    if (participant == kMSInvalidMarkReadyParticipant) {
        if (ThreadLocal::IsCJProcessor()) {
            participant = static_cast<size_t>(ProcessorId());
        } else {
            participant = static_cast<size_t>(GetTid());
        }
    }
    return ResolveMSMarkReadyPreferredShard(
        participant, markScheduler_.RegionShardCount());
}

uint64_t MarkSweepCollector::DrainSchedulerSeriallyUnderSTW()
{
    uint64_t completedWU = 0;
    size_t preferredRegionShard = GetCurrentMarkReadyShard();
    size_t regionProbeCursor = preferredRegionShard + 1;
    while (markScheduler_.OutstandingForTermination() != 0) {
        MSTaskLease lease;
        bool claimed = markScheduler_.TryClaim(
            MSMarkWorkerCaps::GCWorker(), true,
            preferredRegionShard, regionProbeCursor, lease);
        CHECK_DETAIL(
            claimed,
            "[MS-GC] TaskLedger nonzero without ready work under STW: "
            "outstanding=%llu",
            static_cast<unsigned long long>(
                markScheduler_.OutstandingForTermination()));
        MSWorkUnitCounter work;
        RunSchedulerTask(lease, work);
        completedWU = SaturatingMSWUAdd(completedWU, work.Total());
    }
    CHECK_DETAIL(
        markScheduler_.ReadyEmptyForTermination(),
        "[MS-GC] ready stack non-empty with zero TaskLedger");
    cycleCompletedMarkWU_.fetch_add(completedWU, std::memory_order_relaxed);
    return completedWU;
}

void MarkSweepCollector::RunTerminationDrainLaneUnderSTW(
    size_t participant, MSTerminationDrainLaneStats& stats)
{
    size_t previousParticipant = g_msMarkReadyParticipant;
    g_msMarkReadyParticipant = participant;
    size_t preferredRegionShard = GetCurrentMarkReadyShard();
    size_t regionProbeCursor = preferredRegionShard + 1;
    bool preferObject = true;
    for (;;) {
        uint64_t observedProgressEpoch = markWorkEpoch_.Load();
        MSTaskLease lease;
        if (markScheduler_.TryClaim(
                MSMarkWorkerCaps::GCWorker(), preferObject,
                preferredRegionShard, regionProbeCursor, lease)) {
            preferObject = !preferObject;
            MSWorkUnitCounter work;
            RunSchedulerTask(lease, work);
            stats.workUnits = SaturatingMSWUAdd(
                stats.workUnits, work.Total());
            if (markScheduler_.OutstandingForTermination() == 0) {
                markWorkEpoch_.Notify();
            }
            continue;
        }

        // Another termination lane can own the only ledger-protected task.
        // Unlike the serial STW drain, a nonzero ledger with no ready token is
        // therefore a transient state, not an invariant violation.
        if (markScheduler_.OutstandingForTermination() == 0) {
            break;
        }
        (void)markWorkEpoch_.WaitForChange(
            observedProgressEpoch,
            std::chrono::microseconds(kMSTerminationDrainWaitUs));
    }
    g_msMarkReadyParticipant = previousParticipant;
}

MarkSweepCollector::MSTerminationDrainStats
MarkSweepCollector::DrainSchedulerWithHelpersUnderSTW()
{
    MSTerminationDrainStats stats;
    uint64_t startTasks = markScheduler_.OutstandingForTermination();

    size_t configuredThreads = std::min(
        static_cast<size_t>(std::max<int32_t>(
            1, CangjieRuntime::GetGCParam().gcThreads)),
        CangjieRuntime::GetMSGCThreadCapacity());
    size_t maxHelpers = configuredThreads > 1
        ? configuredThreads - 1
        : 0;
    GCThreadPool* threadPool = GetThreadPool();
    if (!markPoolOwnedForCycle_ || threadPool == nullptr) {
        maxHelpers = 0;
    } else {
        maxHelpers = std::min(
            maxHelpers,
            static_cast<size_t>(threadPool->GetMaxThreadNum()));
    }
    size_t policyWorkWords = static_cast<size_t>(std::min<uint64_t>(
        markScheduler_.OutstandingWorkHint(), std::numeric_limits<size_t>::max()));
    size_t helperCount = ComputeMSMarkTerminationDrainHelpers(
        static_cast<size_t>(std::min<uint64_t>(startTasks, std::numeric_limits<size_t>::max())),
        policyWorkWords, maxHelpers);
    if (helperCount == 0) {
        stats.main.workUnits = DrainSchedulerSeriallyUnderSTW();
        stats.workUnits = stats.main.workUnits;
        return stats;
    }

    std::vector<MSTerminationDrainLaneStats> helperStats(helperCount);
    std::vector<std::unique_ptr<HeapWork>> stagedTasks;
    stagedTasks.reserve(helperCount);
    bool allocationFailed = false;
    for (size_t helperId = 0; helperId < helperCount; ++helperId) {
        std::unique_ptr<HeapWork> task(new (std::nothrow) LambdaWork(
            [this, &helperStats, helperId](size_t) {
                RunTerminationDrainLaneUnderSTW(
                    markReadyProcessorCount_ + 1 + helperId,
                    helperStats[helperId]);
            }));
        if (task == nullptr) {
            allocationFailed = true;
            break;
        }
        stagedTasks.push_back(std::move(task));
    }
    if (allocationFailed) {
        stats.main.workUnits = DrainSchedulerSeriallyUnderSTW();
        stats.workUnits = stats.main.workUnits;
        return stats;
    }

    threadPool->SetMaxActiveThreadNum(
        static_cast<int32_t>(helperCount));
    for (std::unique_ptr<HeapWork>& task : stagedTasks) {
        threadPool->AddWork(task.release());
    }
    threadPool->Start();

    RunTerminationDrainLaneUnderSTW(
        markReadyProcessorCount_, stats.main);

    threadPool->WaitFinish();
    for (const MSTerminationDrainLaneStats& helper : helperStats) {
        stats.helpers.workUnits = SaturatingMSWUAdd(
            stats.helpers.workUnits, helper.workUnits);
    }
    stats.workUnits = SaturatingMSWUAdd(
        stats.main.workUnits, stats.helpers.workUnits);
    if (stats.workUnits != 0) {
        cycleCompletedMarkWU_.fetch_add(
            stats.workUnits, std::memory_order_relaxed);
    }
    CHECK_DETAIL(
        markScheduler_.OutstandingForTermination() == 0 &&
        markScheduler_.ReadyEmptyForTermination(),
        "[MS-GC] parallel MarkTermination drain did not reach quiescence: "
        "outstanding=%llu readyEmpty=%d",
        static_cast<unsigned long long>(
            markScheduler_.OutstandingForTermination()),
        markScheduler_.ReadyEmptyForTermination());
    return stats;
}

void MarkSweepCollector::VerifyMarkSchedulerQuiescence(
    const char* closureName)
{
    size_t badRegions = 0;
    size_t strandedPendingRegions = 0;
    size_t nonIdleRegions = 0;
    size_t unscannedObjects = 0;
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
    }
    CHECK_DETAIL(
        badRegions == 0,
         "[MS-GC] Mark scheduler reached false fixed point: closure=%s "
         "badRegions=%zu pendingRegions=%zu "
         "nonIdleRegions=%zu unscannedObjects=%zu",
         closureName, badRegions, strandedPendingRegions,
         nonIdleRegions, unscannedObjects);
}

void MarkSweepCollector::EnumAndMarkRoots()
{
    // Root enumeration only stages raw candidates. One GC-owned MarkWordBatch
    // below performs the white->grey transition and task publication.
    // TransitionAllMutatorsToGCPhase() has already completed every existing
    // mutator's root scan. The AllocBufferMS registry lock protects buffer
    // lifetime while roots are merged, so reacquiring the mutator-management
    // write lock here only introduces reader/writer contention.
    MergeRootsFromAllocBuffers(rootSnapshot_);

    WorkStack globalRoots = NewWorkStack();
    EnumAllNonMutatorRootsSerial(globalRoots);
    EnumAllExportRoots(globalRoots);
    while (!globalRoots.empty()) {
        rootSnapshot_.Append(globalRoots.back());
        globalRoots.pop_back();
    }
}

void MarkSweepCollector::PublishRootStaging()
{
    rootSnapshot_.Deduplicate([](BaseObject* obj) {
        return Heap::IsHeapAddress(obj);
    });
    rootSnapshot_.Publish([this](const std::vector<BaseObject*>& roots) {
        PublishRootRange(roots, 0, roots.size());
    });
}

void MarkSweepCollector::PublishRootRange(
    const std::vector<BaseObject*>& roots, size_t begin, size_t end)
{
    MSMarkWordBatch batch;
    InitializeGCMarkBatch(batch);
    MSMarkPublicationContext& publication = batch.Publication();
    MSWorkUnitCounter work;
    for (size_t i = begin; i < end; ++i) {
        BaseObject* obj = roots[i];
        AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MANAGED>(
            obj, batch, work, publication);
    }
    FlushMarkBatch(batch, work, publication);
    cycleCompletedMarkWU_.fetch_add(work.Total(), std::memory_order_relaxed);
}

void MarkSweepCollector::MergeRootsFromAllocBuffers(WorkStack& workStack)
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->VisitAllocBufferMS([&workStack](AllocBufferMS& buffer) {
            buffer.MergeRoots(workStack);
        });
    }
}

void MarkSweepCollector::MergeRootsFromAllocBuffers(MSRootSnapshot& roots)
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->VisitAllocBufferMS([&roots](AllocBufferMS& buffer) {
            buffer.MergeRoots(roots.Roots());
        });
    }
}

// ============================================================================
// Step 2.2: ConcurrentMark
// ============================================================================

void MarkSweepCollector::ConcurrentMark()
{
    uint64_t markStartUs = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- ConcurrentMark begin ---");
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_CONCURRENT_MARK, true);
    markWorkerControl_.Start(0);

    MSMarkAssistPlan assistPlan;
    MSMarkPressure pressure = ActivateMarkAssistForCurrentCycle(assistPlan);
    RunSchedulerMarkLoop(
        assistPlan.baseRatio, assistPlan.maxMultiplier, pressure);
    assistWorkPerByteScaled_.store(0, std::memory_order_release);
    cycleStats_.concurrentMarkUs = TimeUtil::MicroSeconds() - markStartUs;
    VLOG(REPORT, "[MS-GC] --- ConcurrentMark end: total=%sus ---",
         Pretty(cycleStats_.concurrentMarkUs).Str());
}

void MarkSweepCollector::UpdateMarkAssistPlan()
{
    nextMarkAssistPlan_.baseRatio = ApplyMSAssistDisableOverride(
        ComputeMSAssistRatioQ16(
            markPacer_.GetWorkDensityQ16(), MSMarkPressure::NORMAL, 1),
        CangjieRuntime::IsMSAssistDisabled());
    nextMarkAssistPlan_.maxMultiplier = std::max<size_t>(
        1, CangjieRuntime::GetGCParam().msAssistMaxMultiplier);
}

MSMarkPressure MarkSweepCollector::ActivateMarkAssistForCurrentCycle(
    MSMarkAssistPlan& assistPlan)
{
    assistPlan = nextMarkAssistPlan_;
    MSMarkPressure pressure = gcReason == GC_REASON_OOM
        ? MSMarkPressure::CRITICAL
        : MSMarkPressure::NORMAL;
    size_t multiplier = ComputeMSMarkInitialAssistMultiplier(
        assistPlan.maxMultiplier, pressure);
    uint64_t ratio = SaturatingMSWUMul(assistPlan.baseRatio, multiplier);
    assistWorkPerByteScaled_.store(ratio, std::memory_order_release);
    return pressure;
}

void MarkSweepCollector::RunSchedulerMarkLoop(
    uint64_t baseAssistRatio, size_t maxAssistMultiplier,
    MSMarkPressure pressure)
{
    // Step 1: claim the shared GC worker pool for this cycle and establish
    // the initial controller state. The lease remains held through
    // MarkTermination so its STW closure can reuse the same helpers.
    bool markPoolOwned = false;
    size_t maxHelpers = AcquireMarkWorkerPool(markPoolOwned);
    MSMarkControllerState markController(
        markScheduler_.OutstandingHint(), markScheduler_.OutstandingWorkHint(),
        maxHelpers, maxAssistMultiplier, pressure);
    MSMarkPressureGate pressureGate(pressure);
    StartMarkWorkers(maxHelpers, markController.HelperTarget());

    uint64_t markBudgetUs = CangjieRuntime::GetGCParam().msMarkTimeBudget;
    markBudgetUs = markBudgetUs == 0
        ? 0
        : std::max<uint64_t>(1, markBudgetUs / 1000);
    size_t preferredRegionShard = GetCurrentMarkReadyShard();
    size_t regionProbeCursor = preferredRegionShard + 1;
    uint64_t mainCompletedWU = 0;
    size_t markControlSlices = 0;
    uint64_t markLoopStartUs = TimeUtil::MicroSeconds();

    // Step 2: GC main scans one bounded scheduler slice at a time. Workers
    // and mutator assists publish into the same scheduler concurrently.
    for (;;) {
        uint64_t observedProgressEpoch = markWorkEpoch_.Load();
        MSMarkSliceResult slice = markScheduler_.RunMarkSlice(
            MSMarkWorkerCaps::GCWorker(), preferredRegionShard,
            regionProbeCursor,
            MS_WU_UNLIMITED_BUDGET,
            MS_WU_MAIN_WALL_BUDGET_US,
            [this](
                MSTaskLease& lease, MSWorkUnitCounter& work) {
                RunSchedulerTask(lease, work);
            });
        mainCompletedWU = SaturatingMSWUAdd(
            mainCompletedWU, slice.completedWU);

        uint64_t outstanding = markScheduler_.OutstandingHint();
        MSMarkWorkerControl::Snapshot control =
            markWorkerControl_.GetSnapshot();
        if (HasReachedMarkSchedulerQuiescence(
                outstanding, control,
                markScheduler_.ReadyEmptyForTermination())) {
            QuiesceMarkWorkers();
            break;
        }

        // Step 3: periodically adjust helper width and assist pacing from
        // scheduler backlog; preserve the fixed sampling cadence.
        if (++markControlSlices >= MS_MARK_CONTROL_SLICES_PER_UPDATE) {
            markControlSlices = 0;
            uint64_t outstandingWork =
                markScheduler_.OutstandingWorkHint();
            uint64_t elapsedMarkUs = std::max<uint64_t>(
                1, TimeUtil::MicroSeconds() - markLoopStartUs);
            UpdateMarkController(
                markController, pressureGate, baseAssistRatio,
                outstanding, outstandingWork, control, elapsedMarkUs);
        }

        if (HasReachedMarkTimeBudget(
                TimeUtil::MicroSeconds() - markLoopStartUs, markBudgetUs)) {
            QuiesceMarkWorkers();
            break;
        }

        // Step 4: a zero-result slice may race a publisher. Wait for the
        // progress epoch instead of repeatedly polling an empty scheduler.
        if (slice.completedTasks == 0) {
            (void)markWorkEpoch_.WaitForChange(
                observedProgressEpoch,
                std::chrono::microseconds(kMSMarkProgressWaitUs));
        }
    }
    FinishSchedulerMarkLoop(mainCompletedWU, markPoolOwned);
}

size_t MarkSweepCollector::AcquireMarkWorkerPool(bool& markPoolOwned)
{
    markPoolOwned = false;
    size_t configuredThreads = std::min(
        static_cast<size_t>(std::max<int32_t>(
            1, CangjieRuntime::GetGCParam().gcThreads)),
        CangjieRuntime::GetMSGCThreadCapacity());
    size_t maxHelpers = configuredThreads > 1 ? configuredThreads - 1 : 0;
    GCThreadPool* threadPool = GetThreadPool();
    if (threadPool == nullptr || maxHelpers == 0) {
        return 0;
    }

    maxHelpers = std::min(
        maxHelpers, static_cast<size_t>(threadPool->GetMaxThreadNum()));
    if (maxHelpers == 0 ||
        !collectorResources.AcquireMSGCWorkerPoolForMark()) {
        return 0;
    }
    markPoolOwned = true;
    return maxHelpers;
}

void MarkSweepCollector::UpdateMarkController(
    MSMarkControllerState& markController,
    MSMarkPressureGate& pressureGate,
    uint64_t baseAssistRatio, uint64_t outstanding,
    uint64_t outstandingWork,
    const MSMarkWorkerControl::Snapshot& control,
    uint64_t elapsedMarkUs)
{
    size_t previousHelperTarget = markController.HelperTarget();
    size_t previousAssistMultiplier = markController.AssistMultiplier();
    MSMarkPressure elapsedPressure = ClassifyMSMarkElapsedPressure(
        elapsedMarkUs, outstanding != 0 || outstandingWork != 0);
    markController.EscalatePressure(
        pressureGate.Observe(elapsedPressure), outstanding, outstandingWork);
    markController.Observe(outstanding, outstandingWork);

    size_t targetHelpers = markController.HelperTarget();
    if (targetHelpers != control.targetHelpers) {
        markWorkerControl_.SetTargetHelpers(targetHelpers);
    }

    size_t targetAssistMultiplier = markController.AssistMultiplier();
    uint64_t targetAssistRatio = SaturatingMSWUMul(
        baseAssistRatio, targetAssistMultiplier);
    uint64_t currentAssistRatio =
        assistWorkPerByteScaled_.load(std::memory_order_relaxed);
    if (targetAssistRatio != currentAssistRatio) {
        assistWorkPerByteScaled_.store(
            targetAssistRatio, std::memory_order_release);
    }

    if (targetHelpers != previousHelperTarget ||
        targetAssistMultiplier != previousAssistMultiplier) {
        VLOG(REPORT,
             "[MS-GC] Mark control: tasks=%llu workWords=%llu "
             "filteredWorkWords=%llu helpers=%zu->%zu "
             "assistMultiplier=%zu->%zu elapsed=%lluus pressure=%u",
             static_cast<unsigned long long>(outstanding),
             static_cast<unsigned long long>(outstandingWork),
             static_cast<unsigned long long>(markController.FilteredWorkWords()),
             previousHelperTarget, targetHelpers,
             previousAssistMultiplier, targetAssistMultiplier,
             static_cast<unsigned long long>(elapsedMarkUs),
             static_cast<unsigned int>(elapsedPressure));
    }
}

void MarkSweepCollector::FinishSchedulerMarkLoop(
    uint64_t mainCompletedWU, bool markPoolOwned)
{
    if (mainCompletedWU != 0) {
        cycleCompletedMarkWU_.fetch_add(
            mainCompletedWU, std::memory_order_relaxed);
    }
    CHECK_DETAIL(!markPoolOwnedForCycle_,
                 "[MS-GC] retained Mark worker-pool ownership across cycles");
    markPoolOwnedForCycle_ = markPoolOwned;
}

void MarkSweepCollector::FinalizeConcurrentMarkAccounting()
{
    if (markPoolOwnedForCycle_) {
        collectorResources.ReleaseMSGCWorkerPool(
            MSGCWorkerPoolOwner::MARK);
        markPoolOwnedForCycle_ = false;
    }

    FoldActiveGCMarkAssistWork();
    uint64_t assistCompletedWU =
        markAssistCompletedWU_.load(std::memory_order_relaxed);
    if (assistCompletedWU != 0) {
        cycleCompletedMarkWU_.fetch_add(
            assistCompletedWU, std::memory_order_relaxed);
    }

}


void MarkSweepCollector::StartMarkWorkers(size_t maxHelpers, size_t initialHelpers)
{
    markWorkerCount_ = maxHelpers;
    markWorkerCompletedWU_.assign(maxHelpers, 0);
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
    g_msMarkReadyParticipant = markReadyProcessorCount_ + 1 + workerId;
    size_t preferredRegionShard = GetCurrentMarkReadyShard();
    size_t regionProbeCursor = preferredRegionShard + 1;
    uint64_t helperWU = 0;
    while (markWorkerControl_.WaitUntilEligible(workerId)) {
        if (!markWorkerControl_.TryEnterWorker(workerId)) {
            continue;
        }

        uint64_t observedProgressEpoch = markWorkEpoch_.Load();
        while (markWorkerControl_.CanContinueWorker(workerId)) {
            observedProgressEpoch = markWorkEpoch_.Load();
            MSMarkSliceResult slice = markScheduler_.RunMarkSlice(
                MSMarkWorkerCaps::GCWorker(), preferredRegionShard,
                regionProbeCursor,
                MS_WU_UNLIMITED_BUDGET,
                MS_WU_HELPER_WALL_BUDGET_US,
                [this](
                    MSTaskLease& lease, MSWorkUnitCounter& work) {
                    RunSchedulerTask(lease, work);
                });
            if (slice.completedTasks == 0) {
                break;
            }
            helperWU = SaturatingMSWUAdd(
                helperWU, slice.completedWU);
        }

        markWorkerControl_.ExitWorker();
        if (markWorkerControl_.CanContinueWorker(workerId)) {
            (void)markWorkEpoch_.WaitForChange(
                observedProgressEpoch, std::chrono::microseconds(kMSMarkProgressWaitUs));
        }
    }
    CHECK_DETAIL(
        workerId < markWorkerCompletedWU_.size(),
        "[MS-GC] Mark helper WU slot overflow worker=%zu slots=%zu",
        workerId, markWorkerCompletedWU_.size());
    markWorkerCompletedWU_[workerId] = helperWU;
    g_msMarkReadyParticipant = kMSInvalidMarkReadyParticipant;
}

void MarkSweepCollector::QuiesceMarkWorkers()
{
    markWorkerControl_.BeginQuiesce();
    markWorkerControl_.WaitForNoInFlight();
    markWorkerControl_.Stop();
    if (markWorkerCount_ != 0) {
        GetThreadPool()->WaitFinish();
    }
    uint64_t helperWU = 0;
    for (size_t i = 0; i < markWorkerCompletedWU_.size(); ++i) {
        helperWU = SaturatingMSWUAdd(
            helperWU, markWorkerCompletedWU_[i]);
        markWorkerCompletedWU_[i] = 0;
    }
    if (helperWU != 0) {
        cycleCompletedMarkWU_.fetch_add(
            helperWU, std::memory_order_relaxed);
    }
}


// ============================================================================
// MSMarkAssist: consume Region tasks in WorkUnit-bounded slices.
// ============================================================================
size_t MarkSweepCollector::MSMarkAssist(
    AllocBufferMS& allocBuffer, size_t debtWU)
{
    if (debtWU == 0) {
        return 0;
    }

    MSMarkWorkerControl::Snapshot control = markWorkerControl_.GetSnapshot();
    uint64_t outstandingTasks = markScheduler_.OutstandingHint();
    uint64_t outstandingWorkWords = markScheduler_.OutstandingWorkHint();
    if (!ShouldAdmitMSMarkAssist(
            outstandingTasks, outstandingWorkWords, control.targetHelpers)) {
        return 0;
    }

    if (!markWorkerControl_.TryEnterAssist()) {
        return 0;
    }

    uint64_t budgetWU = ComputeMSMarkAssistSliceBudget(debtWU);
    MSMarkSliceResult slice = markScheduler_.RunMarkAssistSlice(
        GetCurrentMarkReadyShard(),
        allocBuffer.GetGCMarkAssistProbeCursor(), budgetWU,
        MS_WU_ASSIST_WALL_BUDGET_US,
        [this](
            MSTaskLease& lease, MSWorkUnitCounter& work) {
            RunSchedulerTask(lease, work);
        });
    allocBuffer.RecordGCMarkAssistWork(slice.completedWU);
    // Publish owner-local work before dropping the in-flight claim. Quiesce
    // can fold it once assistsInFlight reaches zero.
    markWorkerControl_.ExitAssist();
    return static_cast<size_t>(
        std::min<uint64_t>(
            slice.completedWU, std::numeric_limits<size_t>::max()));
}

// ============================================================================
// Step 2.3: MarkTermination (STW)
// ============================================================================

void MarkSweepCollector::PublishOrdinaryMarkTasksUnderSTW(
    CentralRegionPool& centralPool)
{
    // The worker and mutator-assist claims have quiesced before this STW
    // phase. Publish their private barrier entries and every residual small
    // Region before one ordinary scheduler drain consumes the combined work.
    FlushAllBarrierMarkBuffersUnderSTW(centralPool);
    centralPool.VisitMarkPendingRegionsUnderSTW([this](MSRegionInfo* region) {
        CHECK_DETAIL(region != nullptr &&
                         region->GetRegionKind() == MSRegionKind::SMALL &&
                         region->GetScanClass() == MSScanClass::HAS_REF,
                     "[MS-GC] invalid Region in pending-mark publication");
        if (region->LoadPendingMarkWordsAcquire() == 0) {
            return;
        }
        MSScanState state = region->GetScanState();
        CHECK_DETAIL(state != MSScanState::OWNED,
                     "[MS-GC] owned Region after Mark worker quiescence: idx=%zu",
                     MSRegionInfoArray::GetRegionIdx(region));
        if (state == MSScanState::IDLE &&
            markScheduler_.TryPublishRegion(region, GetCurrentMarkReadyShard())) {
            markWorkEpoch_.Notify();
        }
    });
}

void MarkSweepCollector::PublishFinalizerResurrectionRootsUnderSTW(
    MSWorkUnitCounter& resurrectionPublishWork)
{
    PrepareFinalizersForResurrection();

    MSMarkWordBatch resurrectionBatch;
    InitializeGCMarkBatch(resurrectionBatch);
    MSMarkPublicationContext& resurrectionPublication = resurrectionBatch.Publication();
    VisitFinalizerRoots([this, &resurrectionBatch, &resurrectionPublication,
                         &resurrectionPublishWork](ObjectRef& ref) {
        AddRefToMarkBatch<MSMarkRefSource::DEFENSIVE>(
            ref.object, resurrectionBatch, resurrectionPublishWork, resurrectionPublication);
    });
    FlushMarkBatch(resurrectionBatch, resurrectionPublishWork, resurrectionPublication);
    cycleCompletedMarkWU_.fetch_add(resurrectionPublishWork.Total(), std::memory_order_relaxed);
}

void MarkSweepCollector::MarkTermination(bool worldStopped)
{
    MSWorkUnitCounter resurrectionPublishWork;
    uint64_t stwStartUs = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- MarkTermination begin ---");

    auto runUnderSTW = [&]() {
        RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
        CentralRegionPool* centralPool = space.GetCentralPool();
        CHECK(centralPool != nullptr);

        activeCollector_.store(nullptr, std::memory_order_release);

        // 1. Publish every ordinary residual task before the scheduler scans
        // the combined barrier and Region work to a stable ordinary closure.
        PublishOrdinaryMarkTasksUnderSTW(*centralPool);
        (void)DrainSchedulerWithHelpersUnderSTW();

        // 2. Classify finalizers only after ordinary reachability is stable,
        // then publish and scan their resurrection closure.
        PublishFinalizerResurrectionRootsUnderSTW(resurrectionPublishWork);
        (void)DrainSchedulerWithHelpersUnderSTW();

        // 3. Resolve WeakRefs against the final stable bitmap.
        ClearWeakRefsAfterMark();

        // 4. Verify the final bitmap and both scheduler closures only when
        // the diagnostic gate is enabled for this GC cycle.
        if (IsVerifyMarkEnabled()) {
            VerifyMarkCompleteness();
            VerifyMarkSchedulerQuiescence("ordinary");
            VerifyMarkSchedulerQuiescence("resurrection");
        }

        // Restore IDLE inside STW so resumed Mutators observe the idle barrier.
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, false);
    };

    if (worldStopped) {
        runUnderSTW();
    } else {
        // Do not perform telemetry folding, worker-pool release or REPORT logging
        // between one-way quiesce and this stop-the-world handoff.
        ScopedStopTheWorld stw(
            "MS MarkTermination", true,
            GCPhase::GC_PHASE_MS_MARK_TERMINATION);
        runUnderSTW();
    }

    cycleStats_.markTerminationStwUs =
        TimeUtil::MicroSeconds() - stwStartUs;
    VLOG(REPORT, "[MS-GC] --- MarkTermination end: total=%sus ---",
         Pretty(cycleStats_.markTerminationStwUs).Str());
}


void MarkSweepCollector::PrepareFinalizersForResurrection()
{
    std::function<bool(BaseObject*)> finalizable = [this](BaseObject* obj) {
        if (!IsTraceableObjectAddress(obj) || !obj->IsValidObject()) {
            return false;
        }
        return !IsMarkedObject(obj);
    };
    (void)collectorResources.EnqueueFinalizables(finalizable);
}


void MarkSweepCollector::ClearWeakRefsAfterMark()
{
    WeakRefBuffer::Instance().DrainWeakRefs([this](BaseObject* weakRef) {
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
            return;
        }
    });
}

// ============================================================================
// Step 2.4: PostMarkTermination
// Normal cycles run after MarkTermination releases STW; OOM cycles retain the
// outer emergency STW window through this handoff.
// ============================================================================

void MarkSweepCollector::PostMarkTermination()
{
    uint64_t startUs = TimeUtil::MicroSeconds();
    VLOG(REPORT, "[MS-GC] --- PostMarkTermination begin ---");

    // Step 1: retire concurrent-Mark resources and fold completed
    // mutator-assist work into this cycle's pacing total.
    FinalizeConcurrentMarkAccounting();

    // MarkTermination already completed the STW-only semantic closure
    // (finalizers, WeakRefs, barrier transition). The remaining steps only
    // consume the immutable collect-set and publish the next Sweep epoch.
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    CHECK(centralPool != nullptr);

    // Step 2: scan the immutable collect-set once. This small linear
    // reduction stays serial, avoiding task construction, pool arbitration
    // and an additional pre-scan solely to split work into chunks.
    MSMarkedStats markedStats = centralPool->ComputeMarkedStats();
    cycleStats_.trueLiveBytes = markedStats.liveBytes;
    cycleStats_.outsideCentralSmallBytes =
        SaturatingMultiplyMSBytes(
            centralPool->GetSmallSweptRegionCount(), MS_REGION_SIZE);
    cycleStats_.outsideLargeBytes =
        SaturatingMultiplyMSBytes(
            centralPool->GetLargeNextSupplySpanCount(), MS_REGION_SIZE);

    // Step 4: partition the frozen LARGE collect-set after its bitmap
    // statistics are consumed. Live spans join next supply; dead spans are
    // returned directly to FreeManager.
    centralPool->SweepLargeRegions();

    // Step 5: publish the new heap/scavenge thresholds before Sweep.
    // Sweep assist derives its initial runway from this threshold, so the
    // new value must be visible before the epoch becomes claimable.
    UpdateGCStats();

    // Step 6: turn the immutable Mark collect-set into the Sweep backlog.
    // This must follow every bitmap/statistics consumer; regions allocated
    // after PreEnum remain swept allocation supply for the next cycle.
    centralPool->PublishCollectSetForSweep();

    // Step 7: request background Sweep and Scavenge after their respective
    // epochs and thresholds are published. GC completion separately reopens
    // the background processor's GC-active gate.
    BackgroundProcessor& backgroundProcessor = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
    backgroundProcessor.NotifyToSweep(kNoPreferredSweepClass);
    backgroundProcessor.NotifyToScavenge();
    cycleStats_.postMarkTerminationUs = TimeUtil::MicroSeconds() - startUs;
    VLOG(REPORT, "[MS-GC] --- PostMarkTermination end: total=%sus ---",
         Pretty(cycleStats_.postMarkTerminationUs).Str());
}

// ============================================================================
// VerifyMarkCompleteness: after marking, scan every marked object and report
// ref edges that point to an unmarked (soon-to-be-swept) target.
// ============================================================================
void MarkSweepCollector::VerifyMarkCompleteness()
{
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
                         static_cast<void*>(obj), refType, refState,
                         static_cast<void*>(target), tgtState,
                         static_cast<int>(targetTraceable), static_cast<int>(targetValid));
                    std::string slotDesc = DescribeSlotForDiag(obj, field);
                    std::string refLayout = DescribeTypeLayoutForDiag(obj->GetTypeInfo());
                    VLOG(REPORT,
                         "[MS-GC][VERIFY] stale-ref-detail: referrer=%p %s layout={%s}",
                         static_cast<void*>(obj), slotDesc.c_str(), refLayout.c_str());
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
                     static_cast<void*>(obj), refType, refState,
                     static_cast<void*>(target), tgtType, tgtState,
                     static_cast<int>(targetTraceable), static_cast<int>(targetValid));
                if (UNLIKELY(IsVerifyDetailEnabled() && IsInterestingDiagType(obj))) {
                    std::string slotDesc = DescribeSlotForDiag(obj, field);
                    std::string refLayout = DescribeTypeLayoutForDiag(obj->GetTypeInfo());
                    VLOG(REPORT,
                         "[MS-GC][VERIFY] missed-mark-detail: referrer=%p %s layout={%s}",
                         static_cast<void*>(obj), slotDesc.c_str(), refLayout.c_str());
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
    InitializeGCMarkBatch(batch);
    MSMarkPublicationContext& publication = batch.Publication();
    MSWorkUnitCounter work;
    AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MANAGED>(
        obj, batch, work, publication);
    FlushMarkBatch(batch, work, publication);
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

// One REPORT line is emitted after each GC cycle.
void MarkSweepCollector::LogGCEntry(uint64_t gcIndex, GCReason reason)
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    uint32_t sweepCycleId = centralPool != nullptr ? centralPool->GetSweepCycleId() : 0;
    VLOG(REPORT, "[MS-GC] === GC START cycle=%llu === sweepCycleId=%u reason=%u(%s)",
         static_cast<unsigned long long>(gcCycle_), sweepCycleId,
         static_cast<unsigned>(reason), g_gcRequests[reason].name);
    VLOG(REPORT, "[MS-GC] active-region bytes %s B, threshold %s B, gcIndex=%llu",
        Pretty(space.GetHeapInUseBytes()).Str(),
         Pretty(GetGCStats().GetThreshold()).Str(),
         static_cast<unsigned long long>(gcIndex));
    MSRegionManager* msRegionManager = space.GetMSRegionManager();
    size_t unsweptCount = (centralPool != nullptr) ? centralPool->GetTotalUnsweptCount() : 0;
    size_t emptyCount = (centralPool != nullptr) ? centralPool->GetEmptySweptRegionCount() : 0;
    size_t hotFreeBytes = (msRegionManager != nullptr) ? msRegionManager->GetHotFreeBytes() : 0;
    size_t hotFreeCount = hotFreeBytes / MS_REGION_SIZE;
    VLOG(REPORT, "[MS-GC] unswept: %zu regions, emptyCache: %zu regions, hotFree: %zu regions",
        unsweptCount, emptyCount, hotFreeCount);
}

void MarkSweepCollector::DumpRegionStatsMS()
{
    if (!IsMSGCReportEnabled()) {
        return;
    }
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    uint32_t sweepCycleId = centralPool != nullptr ? centralPool->GetSweepCycleId() : 0;
    VLOG(REPORT, "[MS-GC] --- Total ---");
    VLOG(REPORT, "[MS-GC] region statistics when gc ends (sweepCycleId=%u)", sweepCycleId);
    size_t totalSize = space.GetMaxCapacity();
    size_t activeRegionBytes = space.GetHeapInUseBytes();
    size_t trueLiveBytes = cycleStats_.trueLiveBytes;
    double utilization = activeRegionBytes == 0 ? 0.0 :
        (static_cast<double>(trueLiveBytes) / static_cast<double>(activeRegionBytes)) * 100.0;
    MSRegionManager* msRegionManager = space.GetMSRegionManager();
    size_t currentHeapSize = msRegionManager != nullptr ? msRegionManager->GetCurrentCapacityBytes() : 0;
    VLOG(REPORT, "[MS-GC]   total capacity: %s B, current heap: %s B",
         Pretty(totalSize).Str(), Pretty(currentHeapSize).Str());
    VLOG(REPORT, "[MS-GC]   active-region bytes: %s B, true live: %s B, true-live utilization: %.2f%%",
         Pretty(activeRegionBytes).Str(), Pretty(trueLiveBytes).Str(), utilization);
    if (msRegionManager != nullptr) {
        size_t emptyRegionCount = centralPool != nullptr ? centralPool->GetEmptySweptRegionCount() : 0;
        size_t hotFreeRegionCount = msRegionManager->GetHotFreeBytes() / MS_REGION_SIZE;
        VLOG(REPORT,
             "[MS-GC]   region-accounting: active=%zu regions, empty=%zu regions, hotFree=%zu regions",
             activeRegionBytes / MS_REGION_SIZE, emptyRegionCount, hotFreeRegionCount);
    }
    if (centralPool != nullptr) {
        centralPool->DumpPoolStats();
        uint32_t processorNum = Runtime::CurrentRef() != nullptr
            ? Runtime::Current().GetConcurrencyModel().GetProcessorNum() : 0;
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
             static_cast<unsigned>(cls), pressure.partialSwept, pressure.fullSwept,
             pressure.unswept, clsTotal);
    }
}

void MarkSweepCollector::LogGCSummary(uint64_t gcTimeUs)
{
    uint64_t accountedUs = cycleStats_.preEnumRootsUs +
                           cycleStats_.enumRootsUs +
                           cycleStats_.concurrentMarkUs +
                           cycleStats_.markTerminationStwUs +
                           cycleStats_.postMarkTerminationUs;
    uint64_t otherUs = (gcTimeUs > accountedUs) ? (gcTimeUs - accountedUs) : 0;

    VLOG(REPORT,
         "[MS-GC] GC total=%sus PreEnum=%sus EnumRoots=%sus ConcurrentMark=%sus "
         "MarkTerminationSTW=%sus PostMarkTermination=%sus Other=%sus",
         Pretty(gcTimeUs).Str(),
         Pretty(cycleStats_.preEnumRootsUs).Str(),
         Pretty(cycleStats_.enumRootsUs).Str(),
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         Pretty(cycleStats_.markTerminationStwUs).Str(),
         Pretty(cycleStats_.postMarkTerminationUs).Str(),
         Pretty(otherUs).Str());
}

// ============================================================================
// MS-GC specific UpdateGCStats: simplified Go-style threshold
// ============================================================================
struct MarkSweepCollector::MSHeapThresholdStateUpdate {
    size_t liveBytes = 0;
    size_t inUseBytes = 0;
    size_t maxHeapBytes = 0;
    size_t softLimitBytes = 0;
    size_t newThreshold = 0;
    bool adaptiveEnabled = false;
    MSAdaptiveHeapGrowthDecision adaptiveDecision;
};

struct MarkSweepCollector::MSScavengeWatermarkStateUpdate {
    size_t newLowBytes = 0;
    size_t newHighBytes = 0;
};

MarkSweepCollector::MSHeapThresholdStateUpdate
MarkSweepCollector::UpdateMSHeapThresholdState(
    RegionSpaceMS& space, const GCStats& gcStats, bool exceptionalBypass)
{
    MSHeapThresholdStateUpdate state;
    size_t oldThreshold = gcStats.GetThreshold();
    size_t oldLastTrueLiveBytes = gcStats.lastTrueLiveBytes;
    state.liveBytes = cycleStats_.trueLiveBytes;
    state.inUseBytes = space.GetHeapInUseBytes();
    state.maxHeapBytes = space.GetMaxCapacity();

    // The raw MSGC target is marked-live bytes * (1 + heapGrowth).
    // Established normal cycles use an inclusive 10% deadband and symmetric
    // 12.5% transition limits. Bootstrap and OOM cycles establish the
    // target directly.
    size_t baseBytes = cycleStats_.trueLiveBytes;
    double configuredHeapGrowth = CangjieRuntime::GetHeapParam().heapGrowth;
    state.adaptiveEnabled = CangjieRuntime::GetGCParam().msAdaptiveGrowth != 0;
    state.softLimitBytes = CangjieRuntime::GetMSSoftHeapLimitBytes();
    size_t gcThresholdCap = state.maxHeapBytes != 0
        ? state.maxHeapBytes
        : CangjieRuntime::GetGCParam().gcThreshold;
    if (state.softLimitBytes != 0) {
        // Preserve a 20% allocation runway beneath the configured heap budget.
        gcThresholdCap = std::min(
            gcThresholdCap,
            state.softLimitBytes - state.softLimitBytes / 5);
    }
    size_t absoluteMinThreshold =
        std::min(gcThresholdCap, static_cast<size_t>(20) * 1024 * 1024);
    size_t minThreshold = ComputeMSHeapThresholdFloor(
        absoluteMinThreshold, gcThresholdCap);
    double maxAdaptiveGrowth = 0.0;
    if (baseBytes != 0 && gcThresholdCap > baseBytes) {
        maxAdaptiveGrowth = static_cast<double>(gcThresholdCap) /
            static_cast<double>(baseBytes) - 1.0;
    }
    uint64_t nowUs = TimeUtil::MicroSeconds();
    bool normalHeu = gcStats.reason == GC_REASON_HEU;
    uint64_t heuPeriodUs = 0;
    if (normalHeu) {
        if (lastHeuGcStartUs_ != 0 && nowUs >= lastHeuGcStartUs_) {
            heuPeriodUs = nowUs - lastHeuGcStartUs_;
        }
        lastHeuGcStartUs_ = nowUs;
    } else if (exceptionalBypass) {
        lastHeuGcStartUs_ = 0;
    }
    MSAdaptiveHeapGrowthInput adaptiveInput;
    adaptiveInput.adaptiveEnabled = state.adaptiveEnabled;
    adaptiveInput.initialGrowth = configuredHeapGrowth;
    adaptiveInput.maxGrowth = maxAdaptiveGrowth;
    adaptiveInput.thresholdBaseBytes = baseBytes;
    adaptiveInput.preSweepInUseBytes = cycleStats_.preSweepInUseBytes;
    adaptiveInput.softLimitBytes = state.softLimitBytes;
    uint64_t gcWallUs = nowUs >= currentGcStartUs_
        ? nowUs - currentGcStartUs_ : 0;
    adaptiveInput.gcWallUs = gcWallUs;
    adaptiveInput.gcPeriodUs = heuPeriodUs;
    adaptiveInput.nowUs = nowUs;
    adaptiveInput.normalHeu = normalHeu;
    adaptiveInput.oom = exceptionalBypass;
    state.adaptiveDecision =
        EvaluateMSAdaptiveHeapGrowth(adaptiveHeapGrowthState_, adaptiveInput);
    bool bypassTransition =
        exceptionalBypass || oldLastTrueLiveBytes == 0 ||
        state.adaptiveDecision.bypassTransition;
    MSHeapThresholdDecision thresholdDecision = EvaluateMSHeapThreshold(
        oldThreshold, baseBytes, state.adaptiveDecision.effectiveGrowth,
        minThreshold, gcThresholdCap, bypassTransition);
    state.newThreshold = thresholdDecision.threshold;
    return state;
}

MarkSweepCollector::MSScavengeWatermarkStateUpdate
MarkSweepCollector::UpdateMSScavengeWatermarkState(
    RegionSpaceMS& space, const GCStats& gcStats,
    const MSHeapThresholdStateUpdate& heapThresholdState)
{
    MSScavengeWatermarkStateUpdate state;
    size_t oldHighBytes = gcStats.GetScavengeTriggerThreshold();
    bool adaptiveEnabled = heapThresholdState.adaptiveEnabled;
    double configuredScavengeGrowth =
        CangjieRuntime::GetGCParam().scavengeGrowth;
    size_t scavengeBudgetBytes = heapThresholdState.softLimitBytes != 0
        ? heapThresholdState.softLimitBytes
        : (heapThresholdState.maxHeapBytes != 0
            ? heapThresholdState.maxHeapBytes
            : CangjieRuntime::GetGCParam().gcThreshold);
    MSRegionManager* regionManager = space.GetMSRegionManager();
    size_t segmentBytes = regionManager == nullptr
        ? MS_REGION_SIZE
        : regionManager->GetSegmentSizeBytes();
    MSScavengeWatermarkDecision scavengeDecision = adaptiveEnabled
        ? EvaluateMSScavengeWatermarks(
            heapThresholdState.newThreshold, cycleStats_.preSweepInUseBytes,
            scavengeBudgetBytes, segmentBytes)
        : EvaluateMSStaticScavengeWatermarks(
            heapThresholdState.newThreshold, cycleStats_.preSweepInUseBytes,
            scavengeBudgetBytes, configuredScavengeGrowth);
    state.newLowBytes = scavengeDecision.retainLowBytes;
    if (!adaptiveEnabled) {
        state.newHighBytes = scavengeDecision.retainHighBytes;
        lastScavengeHighDecayUs_ = 0;
        return state;
    }
    uint64_t watermarkNowUs = TimeUtil::MicroSeconds();
    uint64_t highDecayElapsedUs = 0;
    if (lastScavengeHighDecayUs_ != 0 &&
        scavengeDecision.retainHighBytes < oldHighBytes) {
        highDecayElapsedUs = watermarkNowUs - lastScavengeHighDecayUs_;
    }
    MSScavengeHighWatermarkDecision highDecision =
        StabilizeMSScavengeHighWatermark(
            scavengeDecision.retainHighBytes, state.newLowBytes,
            oldHighBytes, highDecayElapsedUs, segmentBytes,
            heapThresholdState.adaptiveDecision.requestHighContraction);
    state.newHighBytes = highDecision.retainHighBytes;
    if (oldHighBytes == 0 || scavengeDecision.retainHighBytes >= oldHighBytes) {
        lastScavengeHighDecayUs_ = watermarkNowUs;
    } else if (highDecision.decayIntervals != 0) {
        lastScavengeHighDecayUs_ = watermarkNowUs -
            (highDecayElapsedUs % kMSScavengeHighDecayIntervalUs);
    }
    return state;
}

void MarkSweepCollector::UpdateGCStats()
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    GCStats& gcStats = GetGCStats();

    bool exceptionalBypass = gcStats.reason == GC_REASON_OOM;
    size_t oldThreshold = gcStats.GetThreshold();
    MSHeapThresholdStateUpdate heapThresholdState =
        UpdateMSHeapThresholdState(space, gcStats, exceptionalBypass);
    size_t allocatedMarkBytes =
        heapThresholdState.inUseBytes > cycleStats_.markStartInUseBytes
        ? heapThresholdState.inUseBytes - cycleStats_.markStartInUseBytes
        : 0;
    gcStats.lastTrueLiveBytes = heapThresholdState.liveBytes;

    MSScavengeWatermarkStateUpdate scavengeWatermarkState =
        UpdateMSScavengeWatermarkState(space, gcStats, heapThresholdState);
    uint64_t completedWU =
        cycleCompletedMarkWU_.load(std::memory_order_relaxed);
    // Publish the one-cycle outside-set correction before the release-store of
    // the GC trigger threshold. Region acquisition reads the threshold first,
    // then pairs it with this snapshot when it computes HEU pressure.
    size_t outsideMarkBytes = SaturatingAddMSBytes(
        cycleStats_.outsideCentralSmallBytes, cycleStats_.outsideLargeBytes);
    gcStats.SetLastMarkOutsideBytes(outsideMarkBytes);
    gcStats.SetScavengeThreshold(scavengeWatermarkState.newLowBytes);
    gcStats.SetScavengeTriggerThreshold(scavengeWatermarkState.newHighBytes);
    gcStats.SetThreshold(heapThresholdState.newThreshold);
    markPacer_.UpdateFullCycle(completedWU, allocatedMarkBytes);
    UpdateMarkAssistPlan();
    VLOG(REPORT,
         "[MS-GC] GC threshold: live=%zuB inUse=%zuB heap=%zuB->%zuB "
         "scavenge=%zuB/%zuB outside=%zuB",
         heapThresholdState.liveBytes, heapThresholdState.inUseBytes,
         oldThreshold, heapThresholdState.newThreshold,
         scavengeWatermarkState.newLowBytes,
         scavengeWatermarkState.newHighBytes, outsideMarkBytes);
    TRACE_COUNT("CJRT_post_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
