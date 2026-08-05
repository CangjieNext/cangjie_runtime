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
#include <memory>
#include <string>
#include <thread>
#include "Base/CString.h"
#include "Base/EnvParam.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/SysCall.h"
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
    CHECK_DETAIL(
        markBatchEpoch_ != MSRegionInfo::NO_COLLECT_EPOCH &&
        markBatchAllocPlaneBase_ != 0 && markBatchMarkPlaneBase_ != 0,
        "[MS-GC] GC MarkWordBatch initialized without a cycle bitmap context");
    batch.SetMarkEpoch(markBatchEpoch_);
    batch.SetBitmapPlaneBases(
        markBatchAllocPlaneBase_, markBatchMarkPlaneBase_);
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

    void RecordResolveRejected() const
    {
        RecordMSBarrierRuntimeStat(
            MSBarrierRuntimeStat::OLD_REF_RESOLVE_REJECTED);
    }

    template<typename FlushEntry>
    void Add(uint32_t regionIdx, size_t wordIdx, uint64_t mask,
             FlushEntry&& flushEntry)
    {
        if (UNLIKELY(
                buffer_.pendingBitCount >=
                kMSBarrierMarkBufferPendingBitFlushThreshold)) {
            RecordMSBarrierRuntimeStat(
                MSBarrierRuntimeStat::OLD_REF_THRESHOLD_FLUSH);
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
            RecordMSBarrierRuntimeStat(
                MSBarrierRuntimeStat::OLD_REF_SET_CONFLICT_FLUSH);
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
    Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor().NotifyToScavenge();
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
//   Stop all sweeps, wait for in-flight background/assist/helper work to quiesce, clear
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
    markAssistCompletedWU_.store(0, std::memory_order_relaxed);
    markAssistActiveUs_.store(0, std::memory_order_relaxed);
    markAssistCalls_.store(0, std::memory_order_relaxed);
    markAssistNoTaskCalls_.store(0, std::memory_order_relaxed);
    // GC main follows the P participants in the stable affinity namespace.
    // The hint remains valid through root publication, concurrent Mark and
    // both MarkTermination closure drains on this GC thread.
    g_msMarkReadyParticipant = markReadyProcessorCount_;

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
    CHECK_DETAIL(centralPool != nullptr,
                 "[MS-GC] RegionSpaceMS has no CentralRegionPool in PreEnumRoots");
    {
        uint64_t stopSweepT0 = TimeUtil::MicroSeconds();
        centralPool->StopAndWaitForAllSweep();
        uint64_t stopSweepUs = TimeUtil::MicroSeconds() - stopSweepT0;

        uint64_t clearBitmapUs = 0;
        uint64_t stwT0 = TimeUtil::MicroSeconds();
        {
            ScopedStopTheWorld stw("MS PreEnumRoots clear/reclassify/prepare STW");
            markScheduler_.ResetCycle();
            PrepareBarrierMarkBuffersForNewEpochUnderSTW(*centralPool);
            uint64_t clearBitmapT0 = TimeUtil::MicroSeconds();
            markBatchEpoch_ =
                centralPool->PrepareCollectSetForMarkUnderSTW();
            markBatchAllocPlaneBase_ = static_cast<uintptr_t>(
                __atomic_load_n(
                    &CJ_MRT_MSGCFastPathState.allocPlaneBase,
                    __ATOMIC_RELAXED));
            markBatchMarkPlaneBase_ = static_cast<uintptr_t>(
                __atomic_load_n(
                    &CJ_MRT_MSGCFastPathState.markPlaneBase,
                    __ATOMIC_RELAXED));
            CHECK_DETAIL(
                markBatchAllocPlaneBase_ != 0 &&
                markBatchMarkPlaneBase_ != 0,
                "[MS-GC] PreEnum prepared an empty bitmap context");
            clearBitmapUs = TimeUtil::MicroSeconds() - clearBitmapT0;

            // Keep the old collector gate closed until every prior-epoch
            // scheduler and barrier-buffer state has been discarded.  In
            // particular, an AllocBufferMS that exits while sweep is being
            // stopped must reset a stale buffer instead of flushing it into
            // the old TaskLedger just before ResetCycle().  Mutators remain
            // stopped here, so opening the gate after the new epoch is fully
            // prepared cannot race a barrier publication.
            activeCollector_.store(this, std::memory_order_release);
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
    uint64_t mutatorCountT0 = TimeUtil::MicroSeconds();
    size_t enumMutatorCount = MutatorManager::Instance().GetMutatorCount();
    uint64_t mutatorCountUs = TimeUtil::MicroSeconds() - mutatorCountT0;
    uint64_t poolSetupUs = 0;
    uint64_t phaseTransitionT0 = TimeUtil::MicroSeconds();
    MutatorManager::Instance().TransitionAllMutatorsToGCPhase(
        GCPhase::GC_PHASE_MS_MARK_SETUP);
    uint64_t phaseTransitionUs = TimeUtil::MicroSeconds() - phaseTransitionT0;

    // GC thread enumerates global roots + marks root set.
    rootStaging_.clear();
    uint64_t rootEnumerationT0 = TimeUtil::MicroSeconds();
    EnumAndMarkRoots();
    uint64_t rootEnumerationUs = TimeUtil::MicroSeconds() - rootEnumerationT0;
    uint64_t rootPublishT0 = TimeUtil::MicroSeconds();
    PublishRootStaging();
    uint64_t rootPublishUs = TimeUtil::MicroSeconds() - rootPublishT0;
    uint64_t poolFinishUs = 0;

    cycleStats_.markSetupUs = TimeUtil::MicroSeconds() - phaseStart;
    VLOG(REPORT,
         "[MS-GC] EnumRoots: stage-times mutators=%zu mutatorCount=%sus poolSetup=%sus "
         "phaseTransition=%sus rootEnumeration=%sus rootPublish=%sus poolFinish=%sus "
         "total=%sus roots=%zu",
         enumMutatorCount, Pretty(mutatorCountUs).Str(), Pretty(poolSetupUs).Str(),
         Pretty(phaseTransitionUs).Str(), Pretty(rootEnumerationUs).Str(), Pretty(rootPublishUs).Str(),
         Pretty(poolFinishUs).Str(),
         Pretty(cycleStats_.markSetupUs).Str(), cycleStats_.rootsCount);
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
            batch.RecordResolveRejected();
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
        batch.RecordResolveRejected();
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
    FoldGCMarkAssistTelemetry(allocBuffer);
    FlushBarrierMarkBuffer(allocBuffer.GetBarrierMarkBuffer());
}

void MarkSweepCollector::FoldGCMarkAssistTelemetry(
    AllocBufferMS& allocBuffer)
{
    uint64_t completedWU;
    uint64_t activeUs;
    uint64_t productiveCalls;
    uint64_t noTaskCalls;
    allocBuffer.DrainGCMarkAssistTelemetry(
        completedWU, activeUs, productiveCalls, noTaskCalls);
    if (completedWU != 0) {
        markAssistCompletedWU_.fetch_add(
            completedWU, std::memory_order_relaxed);
    }
    if (activeUs != 0) {
        markAssistActiveUs_.fetch_add(
            activeUs, std::memory_order_relaxed);
    }
    if (productiveCalls != 0) {
        markAssistCalls_.fetch_add(
            productiveCalls, std::memory_order_relaxed);
    }
    if (noTaskCalls != 0) {
        markAssistNoTaskCalls_.fetch_add(
            noTaskCalls, std::memory_order_relaxed);
    }
}

void MarkSweepCollector::FoldActiveGCMarkAssistTelemetry()
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool == nullptr) {
        return;
    }
    centralPool->VisitAllocBufferMS([this](AllocBufferMS& buffer) {
        FoldGCMarkAssistTelemetry(buffer);
    });
}

void MarkSweepCollector::FlushAllBarrierMarkBuffersUnderSTW(
    CentralRegionPool& central, size_t& visitedBuffers,
    size_t& nonEmptyBuffers, size_t& bufferedEntries)
{
    visitedBuffers = 0;
    nonEmptyBuffers = 0;
    bufferedEntries = 0;
    central.VisitAllocBufferMS([this, &visitedBuffers, &nonEmptyBuffers,
                                &bufferedEntries](AllocBufferMS& buffer) {
        ++visitedBuffers;
        MSBarrierMarkBuffer& markBuffer = buffer.GetBarrierMarkBuffer();
        uint32_t occupancy = markBuffer.occupancy;
        if (occupancy == 0) {
            return;
        }
        ++nonEmptyBuffers;
        bufferedEntries += static_cast<size_t>(__builtin_popcount(occupancy));
        FlushBarrierMarkBuffer(markBuffer);
    });
}

void MarkSweepCollector::PrepareBarrierMarkBuffersForNewEpochUnderSTW(
    CentralRegionPool& central)
{
    central.VisitAllocBufferMS([](AllocBufferMS& buffer) {
        MSBarrierMarkBuffer& markBuffer = buffer.GetBarrierMarkBuffer();
        // The buffer has no epoch tag. Anything left while the mark barrier is
        // inactive belongs to the completed cycle and must not be published
        // into the mark plane that PreEnum prepares for the new cycle.
        ResetMSBarrierMarkBuffer(markBuffer);
        buffer.ResetGCMarkAssistCycleState();
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

uint64_t MarkSweepCollector::DrainSchedulerUnderSTW()
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
            uint64_t taskStartUs = TimeUtil::MicroSeconds();
            MSWorkUnitCounter work;
            RunSchedulerTask(lease, work);
            stats.activeUs = SaturatingMSWUAdd(
                stats.activeUs,
                TimeUtil::MicroSeconds() - taskStartUs);
            stats.workUnits = SaturatingMSWUAdd(
                stats.workUnits, work.Total());
            ++stats.completedTasks;
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
        ++stats.waits;
        (void)markWorkEpoch_.WaitForChange(
            observedProgressEpoch,
            std::chrono::microseconds(kMSTerminationDrainWaitUs));
    }
    g_msMarkReadyParticipant = previousParticipant;
}

MarkSweepCollector::MSTerminationDrainStats
MarkSweepCollector::DrainOrdinarySchedulerUnderSTW()
{
    MSTerminationDrainStats stats;
    uint64_t totalStartUs = TimeUtil::MicroSeconds();
    uint64_t startTasks = markScheduler_.OutstandingForTermination();
    stats.startTasks = static_cast<size_t>(std::min<uint64_t>(
        startTasks, std::numeric_limits<size_t>::max()));
    stats.startWorkWords = markScheduler_.OutstandingWorkHint();

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
        stats.startWorkWords, std::numeric_limits<size_t>::max()));
    size_t helperCount = ComputeMSMarkTerminationDrainHelpers(
        stats.startTasks, policyWorkWords, maxHelpers);
    if (helperCount == 0) {
        uint64_t serialStartUs = TimeUtil::MicroSeconds();
        stats.main.workUnits = DrainSchedulerUnderSTW();
        stats.main.activeUs = TimeUtil::MicroSeconds() - serialStartUs;
        stats.workUnits = stats.main.workUnits;
        stats.mainLaneUs = stats.main.activeUs;
        stats.totalUs = TimeUtil::MicroSeconds() - totalStartUs;
        return stats;
    }

    uint64_t setupStartUs = TimeUtil::MicroSeconds();
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
        uint64_t serialStartUs = TimeUtil::MicroSeconds();
        stats.main.workUnits = DrainSchedulerUnderSTW();
        stats.main.activeUs = TimeUtil::MicroSeconds() - serialStartUs;
        stats.workUnits = stats.main.workUnits;
        stats.mainLaneUs = stats.main.activeUs;
        stats.setupUs = serialStartUs - setupStartUs;
        stats.totalUs = TimeUtil::MicroSeconds() - totalStartUs;
        return stats;
    }

    stats.parallel = true;
    stats.helperCount = helperCount;
    threadPool->SetMaxActiveThreadNum(
        static_cast<int32_t>(helperCount));
    for (std::unique_ptr<HeapWork>& task : stagedTasks) {
        threadPool->AddWork(task.release());
    }
    threadPool->Start();
    stats.setupUs = TimeUtil::MicroSeconds() - setupStartUs;

    uint64_t mainLaneStartUs = TimeUtil::MicroSeconds();
    RunTerminationDrainLaneUnderSTW(
        markReadyProcessorCount_, stats.main);
    stats.mainLaneUs = TimeUtil::MicroSeconds() - mainLaneStartUs;

    uint64_t joinStartUs = TimeUtil::MicroSeconds();
    threadPool->WaitFinish();
    stats.joinUs = TimeUtil::MicroSeconds() - joinStartUs;
    for (const MSTerminationDrainLaneStats& helper : helperStats) {
        stats.helpers.workUnits = SaturatingMSWUAdd(
            stats.helpers.workUnits, helper.workUnits);
        stats.helpers.activeUs = SaturatingMSWUAdd(
            stats.helpers.activeUs, helper.activeUs);
        stats.helpers.completedTasks += helper.completedTasks;
        stats.helpers.waits += helper.waits;
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
    stats.totalUs = TimeUtil::MicroSeconds() - totalStartUs;
    return stats;
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

void MarkSweepCollector::EnumAndMarkRoots()
{
    // Root enumeration only stages raw candidates. One GC-owned MarkWordBatch
    // below performs the white->grey transition and task publication.
    // TransitionAllMutatorsToGCPhase() has already completed every existing
    // mutator's root scan. The AllocBufferMS registry lock protects buffer
    // lifetime while roots are merged, so reacquiring the mutator-management
    // write lock here only introduces reader/writer contention.
    MergeRootsFromAllocBuffers(rootStaging_);

    WorkStack globalRoots = NewWorkStack();
    EnumAllNonMutatorRootsSerial(globalRoots);
    EnumAllExportRoots(globalRoots);
    while (!globalRoots.empty()) {
        rootStaging_.push_back(globalRoots.back());
        globalRoots.pop_back();
    }
}

void MarkSweepCollector::PublishRootStaging()
{
    std::vector<BaseObject*>& roots = rootStaging_;
    const size_t rawRootCount = roots.size();
    cycleStats_.rootsCount = rawRootCount;
    rootDedupTable_.Reset(rawRootCount);
    size_t uniqueRootCount = 0;
    for (BaseObject* obj : roots) {
        if (UNLIKELY(!Heap::IsHeapAddress(obj))) {
            continue;
        }
        uintptr_t address = reinterpret_cast<uintptr_t>(obj);
        if (!rootDedupTable_.Insert(address)) {
            continue;
        }
        roots[uniqueRootCount++] = obj;
    }
    roots.resize(uniqueRootCount);

    PublishRootRange(roots, 0, uniqueRootCount);

    VLOG(REPORT,
         "[MS-GC] EnumRoots root-publish: helpers=%zu rawRoots=%zu uniqueRoots=%zu "
         "batchEntries=%zu dedupCapacity=%zu dedupCleared=%zu helperStage=%sus",
         static_cast<size_t>(0), rawRootCount, uniqueRootCount,
         MSMarkWordBatch::kCapacity, rootDedupTable_.Capacity(),
         rootDedupTable_.LastResetClearedSlotCount(), Pretty(0).Str());
    roots.clear();
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

void MarkSweepCollector::MergeRootsFromAllocBuffers(
    std::vector<BaseObject*>& roots)
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        centralPool->VisitAllocBufferMS([&roots](AllocBufferMS& buffer) {
            buffer.MergeRoots(roots);
        });
    }
}

// ============================================================================
// Step 2.2: ConcurrentMark
// ============================================================================

void MarkSweepCollector::ConcurrentMark()
{
    uint64_t concurrentStartUs = TimeUtil::MicroSeconds();
    uint64_t stageStartUs = concurrentStartUs;
    TransitionToGCPhase(GCPhase::GC_PHASE_MS_CONCURRENT_MARK, true);
    cycleStats_.markPhaseTransitionUs =
        TimeUtil::MicroSeconds() - stageStartUs;

    stageStartUs = TimeUtil::MicroSeconds();
    markWorkerControl_.Start(0);

    bool disableAssist = ReadBoolParameter("CJ_MS_DISABLE_ASSIST", false);
    uint64_t baseAssistRatio = ApplyMSAssistDisableOverride(
        ComputeMSAssistRatioQ16(
            markPacer_.GetWorkDensityQ16(), MSMarkPressure::NORMAL,
            1),
        disableAssist);
    size_t maxAssistMultiplier = std::max<size_t>(
        1, CangjieRuntime::GetGCParam().msAssistMaxMultiplier);
    MSMarkPressure pressure =
        gcReason == GC_REASON_OOM
        ? MSMarkPressure::CRITICAL
        : MSMarkPressure::NORMAL;
    uint64_t outstanding = markScheduler_.OutstandingHint();
    size_t assistMultiplier = ComputeMSMarkInitialAssistMultiplier(
        maxAssistMultiplier, pressure);
    uint64_t assistRatio = SaturatingMSWUMul(
        baseAssistRatio, assistMultiplier);
    assistWorkPerByteScaled_.store(assistRatio, std::memory_order_release);
    VLOG(REPORT,
         "[MS-GC] Assist pacing: disabled=%d baseRatio=%llu "
         "effectiveRatio=%llu multiplier=%zu outstanding=%llu",
         static_cast<int>(disableAssist),
         static_cast<unsigned long long>(baseAssistRatio),
         static_cast<unsigned long long>(assistRatio),
         assistMultiplier,
         static_cast<unsigned long long>(outstanding));
    cycleStats_.markPacingSetupUs =
        TimeUtil::MicroSeconds() - stageStartUs;

    RunSchedulerMarkLoop(
        baseAssistRatio, maxAssistMultiplier, pressure);
    assistWorkPerByteScaled_.store(0, std::memory_order_release);
    cycleStats_.concurrentMarkUs =
        TimeUtil::MicroSeconds() - concurrentStartUs;
}

void MarkSweepCollector::RunSchedulerMarkLoop(
    uint64_t baseAssistRatio, size_t maxAssistMultiplier,
    MSMarkPressure pressure)
{
    uint64_t t0 = TimeUtil::MicroSeconds();
    VLOG(REPORT,
         "[MS-GC] --- ConcurrentMark (REGION/OBJECT scheduler) ---");

    size_t configuredThreads = std::min(
        static_cast<size_t>(std::max<int32_t>(1, CangjieRuntime::GetGCParam().gcThreads)),
        CangjieRuntime::GetMSGCThreadCapacity());
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
        uint64_t poolAcquireStartUs = TimeUtil::MicroSeconds();
        markPoolOwned = collectorResources.AcquireMSGCWorkerPoolForMark();
        cycleStats_.markPoolAcquireUs =
            TimeUtil::MicroSeconds() - poolAcquireStartUs;
        if (!markPoolOwned) {
            maxHelpers = 0;
        }
    }

    uint64_t initialOutstanding = markScheduler_.OutstandingHint();
    uint64_t initialOutstandingWork = markScheduler_.OutstandingWorkHint();
    MSMarkControllerState markController(
        initialOutstanding, initialOutstandingWork,
        maxHelpers, maxAssistMultiplier, pressure);
    size_t activeHelpers = markController.HelperTarget();
    cycleStats_.markMaxHelpers = maxHelpers;
    cycleStats_.markInitialHelpers = activeHelpers;
    uint64_t workerStartUs = TimeUtil::MicroSeconds();
    StartMarkWorkers(maxHelpers, activeHelpers);
    cycleStats_.markWorkerStartUs =
        TimeUtil::MicroSeconds() - workerStartUs;

    uint64_t markBudgetNs = CangjieRuntime::GetGCParam().msMarkTimeBudget;
    uint64_t markBudgetUs = markBudgetNs == 0
        ? 0
        : std::max<uint64_t>(1, markBudgetNs / 1000);
    size_t preferredRegionShard = GetCurrentMarkReadyShard();
    size_t regionProbeCursor = preferredRegionShard + 1;
    uint64_t mainCompletedWU = 0;
    uint64_t mainActiveUs = 0;
    uint64_t mainSlices = 0;
    size_t markControlSlices = 0;
    uint64_t markLoopStartUs = TimeUtil::MicroSeconds();
    for (;;) {
        uint64_t observedProgressEpoch = markWorkEpoch_.Load();
        uint64_t sliceStartUs = TimeUtil::MicroSeconds();
        MSMarkSliceResult slice = markScheduler_.RunMarkSlice(
            MSMarkWorkerCaps::GCWorker(), preferredRegionShard,
            regionProbeCursor,
            MS_WU_UNLIMITED_BUDGET,
            MS_WU_MAIN_WALL_BUDGET_US,
            [this](
                MSTaskLease& lease, MSWorkUnitCounter& work) {
                RunSchedulerTask(lease, work);
            });
        uint64_t sliceUs = TimeUtil::MicroSeconds() - sliceStartUs;
        if (slice.completedTasks != 0) {
            mainActiveUs = SaturatingMSWUAdd(
                mainActiveUs, sliceUs);
            ++mainSlices;
        }
        mainCompletedWU = SaturatingMSWUAdd(
            mainCompletedWU, slice.completedWU);

        uint64_t outstanding = markScheduler_.OutstandingHint();
        MSMarkWorkerControl::Snapshot control =
            markWorkerControl_.GetSnapshot();
        if (outstanding == 0 &&
            control.workersInFlight == 0 &&
            control.assistsInFlight == 0 &&
            markScheduler_.ReadyEmptyForTermination()) {
            cycleStats_.markTerminationReason =
                MSMarkTerminationReason::NATURAL;
            cycleStats_.markLoopUs =
                TimeUtil::MicroSeconds() - markLoopStartUs;
            uint64_t quiesceStartUs = TimeUtil::MicroSeconds();
            cycleStats_.markQuiesceResidualTasks =
                QuiesceMarkWorkers();
            cycleStats_.markQuiesceUs =
                TimeUtil::MicroSeconds() - quiesceStartUs;
            break;
        }

        if (++markControlSlices >= MS_MARK_CONTROL_SLICES_PER_UPDATE) {
            markControlSlices = 0;
            uint64_t outstandingWork =
                markScheduler_.OutstandingWorkHint();
            size_t previousHelperTarget =
                markController.HelperTarget();
            size_t previousAssistMultiplier =
                markController.AssistMultiplier();
            markController.Observe(outstanding, outstandingWork);
            size_t targetHelpers = markController.HelperTarget();
            if (targetHelpers != control.targetHelpers) {
                markWorkerControl_.SetTargetHelpers(targetHelpers);
            }

            size_t targetAssistMultiplier =
                markController.AssistMultiplier();
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
                     "assistMultiplier=%zu->%zu",
                     static_cast<unsigned long long>(outstanding),
                     static_cast<unsigned long long>(outstandingWork),
                     static_cast<unsigned long long>(
                         markController.FilteredWorkWords()),
                     previousHelperTarget, targetHelpers,
                     previousAssistMultiplier, targetAssistMultiplier);
            }
        }

        if (markBudgetUs != 0 &&
            TimeUtil::MicroSeconds() - t0 >= markBudgetUs) {
            cycleStats_.markTerminationReason =
                MSMarkTerminationReason::TIME_BUDGET;
            cycleStats_.markLoopUs =
                TimeUtil::MicroSeconds() - markLoopStartUs;
            uint64_t quiesceStartUs = TimeUtil::MicroSeconds();
            cycleStats_.markQuiesceResidualTasks =
                QuiesceMarkWorkers();
            cycleStats_.markQuiesceUs =
                TimeUtil::MicroSeconds() - quiesceStartUs;
            break;
        }

        if (slice.completedTasks == 0) {
            (void)markWorkEpoch_.WaitForChange(
                observedProgressEpoch,
                std::chrono::microseconds(kMSMarkProgressWaitUs));
        }
    }
    cycleStats_.markMainWU = mainCompletedWU;
    cycleStats_.markMainActiveUs = mainActiveUs;
    cycleStats_.markMainSlices = mainSlices;
    cycleStats_.markSchedulerUs = TimeUtil::MicroSeconds() - t0;
    CHECK_DETAIL(!markPoolOwnedForCycle_,
                 "[MS-GC] retained Mark worker-pool ownership across cycles");
    markPoolOwnedForCycle_ = markPoolOwned;
}

void MarkSweepCollector::FinalizeConcurrentMarkAccounting()
{
    if (markPoolOwnedForCycle_) {
        uint64_t poolReleaseStartUs = TimeUtil::MicroSeconds();
        collectorResources.ReleaseMSGCWorkerPool(
            MSGCWorkerPoolOwner::MARK);
        cycleStats_.markPoolReleaseUs =
            TimeUtil::MicroSeconds() - poolReleaseStartUs;
        markPoolOwnedForCycle_ = false;
    }

    FoldActiveGCMarkAssistTelemetry();
    cycleStats_.markAssistWU =
        markAssistCompletedWU_.load(std::memory_order_relaxed);
    cycleStats_.markAssistActiveUs =
        markAssistActiveUs_.load(std::memory_order_relaxed);
    cycleStats_.markAssistCalls =
        markAssistCalls_.load(std::memory_order_relaxed);
    cycleStats_.markAssistNoTaskCalls =
        markAssistNoTaskCalls_.load(std::memory_order_relaxed);
    if (cycleStats_.markMainWU != 0) {
        cycleCompletedMarkWU_.fetch_add(
            cycleStats_.markMainWU, std::memory_order_relaxed);
    }
    if (cycleStats_.markAssistWU != 0) {
        cycleCompletedMarkWU_.fetch_add(
            cycleStats_.markAssistWU, std::memory_order_relaxed);
    }

    uint64_t totalWU = SaturatingMSWUAdd(
        SaturatingMSWUAdd(
            cycleStats_.markMainWU, cycleStats_.markHelperWU),
        cycleStats_.markAssistWU);
    uint64_t participantActiveUs = SaturatingMSWUAdd(
        SaturatingMSWUAdd(
            cycleStats_.markMainActiveUs,
            cycleStats_.markHelperActiveUs),
        cycleStats_.markAssistActiveUs);
    auto rateAsDouble = [](uint64_t rateQ16) {
        return static_cast<double>(rateQ16) /
            static_cast<double>(MS_WU_Q16_ONE);
    };
    auto shareAsDouble = [](uint64_t shareBasisPoints) {
        return static_cast<double>(shareBasisPoints) / 100.0;
    };

    VLOG(REPORT,
         "[MS-GC] ConcurrentMark scheduler: time=%sus termination=%s "
         "quiesceResidual=%zu",
         Pretty(cycleStats_.markSchedulerUs).Str(),
         GetMSMarkTerminationReasonName(cycleStats_.markTerminationReason),
         cycleStats_.markQuiesceResidualTasks);
    VLOG(REPORT,
         "[MS-GC] ConcurrentMark stages: total=%sus phaseTransition=%sus "
         "pacingSetup=%sus poolAcquire=%sus workerStart=%sus "
         "markLoop=%sus quiesce=%sus poolRelease=%sus",
         Pretty(cycleStats_.concurrentMarkUs).Str(),
         Pretty(cycleStats_.markPhaseTransitionUs).Str(),
         Pretty(cycleStats_.markPacingSetupUs).Str(),
         Pretty(cycleStats_.markPoolAcquireUs).Str(),
         Pretty(cycleStats_.markWorkerStartUs).Str(),
         Pretty(cycleStats_.markLoopUs).Str(),
         Pretty(cycleStats_.markQuiesceUs).Str(),
         Pretty(cycleStats_.markPoolReleaseUs).Str());
    VLOG(REPORT,
         "[MS-GC] ConcurrentMark throughput: totalWU=%llu wallRate=%.2fWU/us "
         "participantRate=%.2fWU/us participantActive=%lluus",
         static_cast<unsigned long long>(totalWU),
         rateAsDouble(ComputeMSMarkRateQ16(
             totalWU, cycleStats_.concurrentMarkUs)),
         rateAsDouble(ComputeMSMarkRateQ16(
             totalWU, participantActiveUs)),
         static_cast<unsigned long long>(participantActiveUs));
    VLOG(REPORT,
         "[MS-GC] ConcurrentMark workers: "
         "gcMain={wu=%llu share=%.2f%% active=%lluus rate=%.2fWU/us slices=%llu} "
         "helper={wu=%llu share=%.2f%% active=%lluus rate=%.2fWU/us slices=%llu "
         "max=%zu initial=%zu}",
         static_cast<unsigned long long>(cycleStats_.markMainWU),
         shareAsDouble(ComputeMSMarkShareBasisPoints(
             cycleStats_.markMainWU, totalWU)),
         static_cast<unsigned long long>(cycleStats_.markMainActiveUs),
         rateAsDouble(ComputeMSMarkRateQ16(
             cycleStats_.markMainWU, cycleStats_.markMainActiveUs)),
         static_cast<unsigned long long>(cycleStats_.markMainSlices),
         static_cast<unsigned long long>(cycleStats_.markHelperWU),
         shareAsDouble(ComputeMSMarkShareBasisPoints(
             cycleStats_.markHelperWU, totalWU)),
         static_cast<unsigned long long>(cycleStats_.markHelperActiveUs),
         rateAsDouble(ComputeMSMarkRateQ16(
             cycleStats_.markHelperWU, cycleStats_.markHelperActiveUs)),
         static_cast<unsigned long long>(cycleStats_.markHelperSlices),
         cycleStats_.markMaxHelpers,
         cycleStats_.markInitialHelpers);
    VLOG(REPORT,
         "[MS-GC] ConcurrentMark assist: "
         "wu=%llu share=%.2f%% active=%lluus rate=%.2fWU/us "
         "productive=%llu empty=%llu",
         static_cast<unsigned long long>(cycleStats_.markAssistWU),
         shareAsDouble(ComputeMSMarkShareBasisPoints(
             cycleStats_.markAssistWU, totalWU)),
         static_cast<unsigned long long>(cycleStats_.markAssistActiveUs),
         rateAsDouble(ComputeMSMarkRateQ16(
             cycleStats_.markAssistWU, cycleStats_.markAssistActiveUs)),
         static_cast<unsigned long long>(cycleStats_.markAssistCalls),
         static_cast<unsigned long long>(
             cycleStats_.markAssistNoTaskCalls));
}


void MarkSweepCollector::StartMarkWorkers(size_t maxHelpers, size_t initialHelpers)
{
    markWorkerCount_ = maxHelpers;
    markWorkerCompletedWU_.assign(maxHelpers, 0);
    markWorkerActiveUs_.assign(maxHelpers, 0);
    markWorkerSlices_.assign(maxHelpers, 0);
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
    uint64_t helperActiveUs = 0;
    uint64_t helperSlices = 0;
    while (markWorkerControl_.WaitUntilEligible(workerId)) {
        if (!markWorkerControl_.TryEnterWorker(workerId)) {
            continue;
        }

        uint64_t observedProgressEpoch = markWorkEpoch_.Load();
        while (markWorkerControl_.CanContinueWorker(workerId)) {
            observedProgressEpoch = markWorkEpoch_.Load();
            uint64_t sliceStartUs = TimeUtil::MicroSeconds();
            MSMarkSliceResult slice = markScheduler_.RunMarkSlice(
                MSMarkWorkerCaps::GCWorker(), preferredRegionShard,
                regionProbeCursor,
                MS_WU_UNLIMITED_BUDGET,
                MS_WU_HELPER_WALL_BUDGET_US,
                [this](
                    MSTaskLease& lease, MSWorkUnitCounter& work) {
                    RunSchedulerTask(lease, work);
                });
            uint64_t sliceUs = TimeUtil::MicroSeconds() - sliceStartUs;
            if (slice.completedTasks == 0) {
                break;
            }
            helperActiveUs = SaturatingMSWUAdd(
                helperActiveUs, sliceUs);
            ++helperSlices;
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
        workerId < markWorkerCompletedWU_.size() &&
        workerId < markWorkerActiveUs_.size() &&
        workerId < markWorkerSlices_.size(),
        "[MS-GC] Mark helper WU slot overflow worker=%zu slots=%zu",
        workerId, markWorkerCompletedWU_.size());
    markWorkerCompletedWU_[workerId] = helperWU;
    markWorkerActiveUs_[workerId] = helperActiveUs;
    markWorkerSlices_[workerId] = helperSlices;
    g_msMarkReadyParticipant = kMSInvalidMarkReadyParticipant;
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
    uint64_t helperWU = 0;
    uint64_t helperActiveUs = 0;
    uint64_t helperSlices = 0;
    for (size_t i = 0; i < markWorkerCompletedWU_.size(); ++i) {
        helperWU = SaturatingMSWUAdd(
            helperWU, markWorkerCompletedWU_[i]);
        helperActiveUs = SaturatingMSWUAdd(
            helperActiveUs, markWorkerActiveUs_[i]);
        helperSlices = SaturatingMSWUAdd(
            helperSlices, markWorkerSlices_[i]);
        markWorkerCompletedWU_[i] = 0;
        markWorkerActiveUs_[i] = 0;
        markWorkerSlices_[i] = 0;
    }
    cycleStats_.markHelperWU = helperWU;
    cycleStats_.markHelperActiveUs = helperActiveUs;
    cycleStats_.markHelperSlices = helperSlices;
    if (helperWU != 0) {
        cycleCompletedMarkWU_.fetch_add(
            helperWU, std::memory_order_relaxed);
    }
    return residualTasks;
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

    if (!markWorkerControl_.TryEnterAssist()) {
        return 0;
    }

    uint64_t budgetWU = ComputeMSMarkAssistSliceBudget(debtWU);
    uint64_t assistStartUs = TimeUtil::MicroSeconds();
    MSMarkSliceResult slice = markScheduler_.RunMarkAssistSlice(
        GetCurrentMarkReadyShard(),
        allocBuffer.GetGCMarkAssistProbeCursor(), budgetWU,
        MS_WU_ASSIST_WALL_BUDGET_US,
        [this](
            MSTaskLease& lease, MSWorkUnitCounter& work) {
            RunSchedulerTask(lease, work);
        });
    uint64_t assistUs = TimeUtil::MicroSeconds() - assistStartUs;

    allocBuffer.RecordGCMarkAssistSlice(
        slice.completedWU, assistUs, slice.completedTasks);
    // Publish owner-local work and telemetry before dropping the in-flight
    // claim. Quiesce can fold those fields once assistsInFlight reaches zero.
    markWorkerControl_.ExitAssist();
    return static_cast<size_t>(
        std::min<uint64_t>(
            slice.completedWU, std::numeric_limits<size_t>::max()));
}

// ============================================================================
// Step 2.3: MarkTermination (STW)
// ============================================================================

void MarkSweepCollector::MarkTermination()
{
    struct ClosureStats {
        uint64_t workUnits = 0;
        uint64_t drainUs = 0;
        uint64_t reconcileUs = 0;
        size_t reconcilePasses = 0;
        size_t reconciledRegions = 0;
        size_t visitedRegions = 0;
        MSTerminationDrainStats initialDrain;
    };
    struct StageStats {
        uint64_t stwAcquireUs = 0;
        uint64_t barrierFlushUs = 0;
        uint64_t finalizerClassifyUs = 0;
        uint64_t resurrectionPublishUs = 0;
        uint64_t weakRefUs = 0;
        uint64_t verifyUs = 0;
        uint64_t idleTransitionUs = 0;
        uint64_t sweepPlanUs = 0;
        uint64_t stwBodyUs = 0;
        uint64_t stwResumeUs = 0;
    } stages;

    uint64_t phaseStartUs = TimeUtil::MicroSeconds();
    uint64_t stwStartUs = phaseStartUs;
    uint64_t beforeStwResumeUs = 0;
    uint64_t entryOutstanding = 0;
    uint64_t afterBarrierFlushOutstanding = 0;
    size_t visitedBarrierBuffers = 0;
    size_t nonEmptyBarrierBuffers = 0;
    size_t bufferedBarrierEntries = 0;
    size_t weakRefsCleared = 0;
    size_t weakRefsPreserved = 0;
    size_t resurrectionEntries = 0;
    MSWorkUnitCounter resurrectionPublishWork;
    ClosureStats ordinaryClosure;
    ClosureStats resurrectionClosure;
    STWAcquireStats stwAcquireStats;

    {
        // Do not perform telemetry folding, worker-pool release or REPORT logging
        // between one-way quiesce and this stop-the-world handoff.
        ScopedStopTheWorld stw(
            "MS MarkTermination", true,
            GCPhase::GC_PHASE_MS_MARK_TERMINATION,
            &stwAcquireStats);
        uint64_t stwBodyStartUs = TimeUtil::MicroSeconds();
        stages.stwAcquireUs = stwBodyStartUs - stwStartUs;

        RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
        CentralRegionPool* centralPool = space.GetCentralPool();
        entryOutstanding = markScheduler_.OutstandingHint();

        // Mutators are frozen, so their owner-private entries are now stable.
        uint64_t stageStartUs = TimeUtil::MicroSeconds();
        if (centralPool != nullptr) {
            FlushAllBarrierMarkBuffersUnderSTW(
                *centralPool, visitedBarrierBuffers,
                nonEmptyBarrierBuffers, bufferedBarrierEntries);
        }
        stages.barrierFlushUs = TimeUtil::MicroSeconds() - stageStartUs;
        afterBarrierFlushOutstanding = markScheduler_.OutstandingHint();

        activeCollector_.store(nullptr, std::memory_order_release);

        // Only the ordinary closure needs the full pending-Region
        // reconciliation. It closes producer/owner races left by the preceding
        // concurrent phase. Resurrection is GC-main-only under STW, so its
        // scheduler drain cannot race a concurrent Region owner.
        auto drainOrdinaryClosureToFixedPoint =
            [this, centralPool](ClosureStats& stats) {
                bool firstDrain = true;
                for (;;) {
                    if (firstDrain) {
                        stats.initialDrain =
                            DrainOrdinarySchedulerUnderSTW();
                        stats.workUnits = SaturatingMSWUAdd(
                            stats.workUnits,
                            stats.initialDrain.workUnits);
                        stats.drainUs += stats.initialDrain.totalUs;
                        firstDrain = false;
                    } else {
                        uint64_t drainStartUs =
                            TimeUtil::MicroSeconds();
                        stats.workUnits = SaturatingMSWUAdd(
                            stats.workUnits,
                            DrainSchedulerUnderSTW());
                        stats.drainUs +=
                            TimeUtil::MicroSeconds() - drainStartUs;
                    }
                    if (centralPool == nullptr) {
                        break;
                    }

                    size_t passVisited = 0;
                    size_t passReconciled = 0;
                    uint64_t reconcileStartUs = TimeUtil::MicroSeconds();
                    centralPool->VisitMarkPendingRegionsUnderSTW(
                        [this, &passVisited,
                         &passReconciled](MSRegionInfo* region) {
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
                            if (markScheduler_.TryPublishRegion(
                                    region, GetCurrentMarkReadyShard())) {
                                markWorkEpoch_.Notify();
                            }
                        });
                    stats.reconcileUs +=
                        TimeUtil::MicroSeconds() - reconcileStartUs;
                    stats.visitedRegions += passVisited;
                    stats.reconciledRegions += passReconciled;
                    if (passReconciled == 0) {
                        break;
                    }
                    ++stats.reconcilePasses;
                }
            };

        // 1. Complete the ordinary closure against the concurrent producers.
        drainOrdinaryClosureToFixedPoint(ordinaryClosure);
        stageStartUs = TimeUtil::MicroSeconds();
        VerifyMarkSchedulerQuiescence("ordinary");
        stages.verifyUs += TimeUtil::MicroSeconds() - stageStartUs;

        // 2. Classify the registered Finalizer snapshot in one locked pass.
        stageStartUs = TimeUtil::MicroSeconds();
        PrepareFinalizersForResurrection();
        stages.finalizerClassifyUs =
            TimeUtil::MicroSeconds() - stageStartUs;

        // 3. Mark newly-finalizable roots and drain their strong closure. No
        // second full Region reconciliation is needed: all producers here run
        // synchronously on GC main while the world is stopped.
        stageStartUs = TimeUtil::MicroSeconds();
        MSMarkWordBatch resurrectionBatch;
        InitializeGCMarkBatch(resurrectionBatch);
        MSMarkPublicationContext& resurrectionPublication =
            resurrectionBatch.Publication();
        VisitFinalizerRoots(
            [this, &resurrectionBatch, &resurrectionPublication,
             &resurrectionPublishWork,
             &resurrectionEntries](ObjectRef& ref) {
                ++resurrectionEntries;
                AddRefToMarkBatch<MSMarkRefSource::DEFENSIVE>(
                    ref.object, resurrectionBatch, resurrectionPublishWork,
                    resurrectionPublication);
            });
        FlushMarkBatch(
            resurrectionBatch, resurrectionPublishWork,
            resurrectionPublication);
        cycleCompletedMarkWU_.fetch_add(
            resurrectionPublishWork.Total(), std::memory_order_relaxed);
        stages.resurrectionPublishUs =
            TimeUtil::MicroSeconds() - stageStartUs;

        stageStartUs = TimeUtil::MicroSeconds();
        resurrectionClosure.workUnits = DrainSchedulerUnderSTW();
        resurrectionClosure.drainUs =
            TimeUtil::MicroSeconds() - stageStartUs;
        stageStartUs = TimeUtil::MicroSeconds();
        VerifyMarkSchedulerQuiescence("resurrection");
        stages.verifyUs += TimeUtil::MicroSeconds() - stageStartUs;

        // 4. Resolve WeakRefs against the final stable bitmap.
        stageStartUs = TimeUtil::MicroSeconds();
        ClearWeakRefsAfterMark(weakRefsCleared, weakRefsPreserved);
        stages.weakRefUs = TimeUtil::MicroSeconds() - stageStartUs;

        // 5. Optional full-heap mark verification.
        stageStartUs = TimeUtil::MicroSeconds();
        VerifyMarkCompleteness();
        stages.verifyUs += TimeUtil::MicroSeconds() - stageStartUs;

        // Restore IDLE inside STW so resumed Mutators observe the idle barrier.
        stageStartUs = TimeUtil::MicroSeconds();
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, false);
        stages.idleTransitionUs =
            TimeUtil::MicroSeconds() - stageStartUs;

        stageStartUs = TimeUtil::MicroSeconds();
        MSPostMarkSweepPlan sweepPlan =
            BuildMSPostMarkSweepPlan(gcReason == GC_REASON_OOM);
        if (centralPool != nullptr &&
            sweepPlan.reenableAtMarkTermination) {
            centralPool->ReenableSweepAdmission();
        }
        stages.sweepPlanUs = TimeUtil::MicroSeconds() - stageStartUs;
        beforeStwResumeUs = TimeUtil::MicroSeconds();
        stages.stwBodyUs = beforeStwResumeUs - stwBodyStartUs;
    }

    uint64_t phaseEndUs = TimeUtil::MicroSeconds();
    stages.stwResumeUs = phaseEndUs - beforeStwResumeUs;
    cycleStats_.markTerminationUs = phaseEndUs - phaseStartUs;
    uint64_t stwAcquireAccountedUs = stwAcquireStats.syncMutexWaitUs +
        stwAcquireStats.mutatorLockWaitUs + stwAcquireStats.mutatorCountUs +
        stwAcquireStats.suspensionBroadcastUs + stwAcquireStats.unstoppedSnapshotUs +
        stwAcquireStats.safepointWaitUs + stwAcquireStats.phaseTransitionUs;
    uint64_t stwAcquireOtherUs = stages.stwAcquireUs > stwAcquireAccountedUs
        ? stages.stwAcquireUs - stwAcquireAccountedUs
        : 0;

    uint64_t accountedUs = stages.stwAcquireUs +
        stages.barrierFlushUs + ordinaryClosure.drainUs +
        ordinaryClosure.reconcileUs + stages.finalizerClassifyUs +
        stages.resurrectionPublishUs + resurrectionClosure.drainUs +
        stages.weakRefUs + stages.verifyUs + stages.idleTransitionUs +
        stages.sweepPlanUs + stages.stwResumeUs;
    uint64_t otherUs = cycleStats_.markTerminationUs > accountedUs
        ? cycleStats_.markTerminationUs - accountedUs
        : 0;

    // REPORT output is deliberately deferred until after StartTheWorld.
    VLOG(REPORT, "[MS-GC] --- MarkTermination (STW complete) ---");
    VLOG(REPORT,
         "[MS-GC] [mark-stw-entry] branch=%s gcReason=%s "
         "quiesceResidual=%zu outstanding=%llu afterBarrierFlush=%llu",
         GetMSMarkTerminationReasonName(cycleStats_.markTerminationReason),
         g_gcRequests[gcReason].name,
         cycleStats_.markQuiesceResidualTasks,
         static_cast<unsigned long long>(entryOutstanding),
         static_cast<unsigned long long>(afterBarrierFlushOutstanding));
    VLOG(REPORT,
         "[MS-GC] MarkTermination reconciliation: closure=ordinary "
         "passes=%zu regions=%zu visited=%zu",
         ordinaryClosure.reconcilePasses,
         ordinaryClosure.reconciledRegions,
         ordinaryClosure.visitedRegions);
    VLOG(REPORT,
         "[MS-GC] MarkTermination ordinary drain: "
         "parallel=%d helpers=%zu startTasks=%zu startWorkWords=%llu "
         "mainTasks=%zu mainWU=%llu mainActive=%lluus mainWaits=%zu "
         "helperTasks=%zu helperWU=%llu helperActive=%lluus "
         "helperWaits=%zu setup=%lluus mainLane=%lluus join=%lluus",
         ordinaryClosure.initialDrain.parallel,
         ordinaryClosure.initialDrain.helperCount,
         ordinaryClosure.initialDrain.startTasks,
         static_cast<unsigned long long>(
             ordinaryClosure.initialDrain.startWorkWords),
         ordinaryClosure.initialDrain.main.completedTasks,
         static_cast<unsigned long long>(
             ordinaryClosure.initialDrain.main.workUnits),
         static_cast<unsigned long long>(
             ordinaryClosure.initialDrain.main.activeUs),
         ordinaryClosure.initialDrain.main.waits,
         ordinaryClosure.initialDrain.helpers.completedTasks,
         static_cast<unsigned long long>(
             ordinaryClosure.initialDrain.helpers.workUnits),
         static_cast<unsigned long long>(
             ordinaryClosure.initialDrain.helpers.activeUs),
         ordinaryClosure.initialDrain.helpers.waits,
         static_cast<unsigned long long>(
             ordinaryClosure.initialDrain.setupUs),
         static_cast<unsigned long long>(
             ordinaryClosure.initialDrain.mainLaneUs),
         static_cast<unsigned long long>(
             ordinaryClosure.initialDrain.joinUs));
    VLOG(REPORT,
         "[MS-GC] MarkTermination reconciliation: closure=resurrection "
         "passes=0 regions=0 visited=0 skipped=1");
    VLOG(REPORT,
         "[MS-GC] ProcessWeakRefs(STW): cleared=%zu preserved=%zu",
         weakRefsCleared, weakRefsPreserved);
    VLOG(REPORT,
         "[MS-GC] MarkTermination stwAcquire: total=%sus "
         "syncMutexWait=%sus mutatorLockWait=%sus mutatorCount=%sus "
         "suspensionBroadcast=%sus unstoppedSnapshot=%sus "
         "safepointWait=%sus phaseTransition=%sus stwAcquireOther=%sus "
         "mutators=%zu initialUnstopped=%zu",
         Pretty(stages.stwAcquireUs).Str(),
         Pretty(stwAcquireStats.syncMutexWaitUs).Str(),
         Pretty(stwAcquireStats.mutatorLockWaitUs).Str(),
         Pretty(stwAcquireStats.mutatorCountUs).Str(),
         Pretty(stwAcquireStats.suspensionBroadcastUs).Str(),
         Pretty(stwAcquireStats.unstoppedSnapshotUs).Str(),
         Pretty(stwAcquireStats.safepointWaitUs).Str(),
         Pretty(stwAcquireStats.phaseTransitionUs).Str(),
         Pretty(stwAcquireOtherUs).Str(),
         stwAcquireStats.mutatorCount,
         stwAcquireStats.initialUnstoppedMutators);
    VLOG(REPORT,
         "[MS-GC] MarkTermination stages: total=%sus stwAcquire=%sus "
         "barrierFlush=%sus ordinaryDrain=%sus ordinaryReconcile=%sus "
         "finalizerClassify=%sus resurrectionPublish=%sus "
         "resurrectionDrain=%sus weakRef=%sus verify=%sus "
         "idleTransition=%sus sweepPlan=%sus stwResume=%sus other=%sus "
         "stwBody=%sus buffers=%zu nonEmptyBuffers=%zu bufferedEntries=%zu",
         Pretty(cycleStats_.markTerminationUs).Str(),
         Pretty(stages.stwAcquireUs).Str(),
         Pretty(stages.barrierFlushUs).Str(),
         Pretty(ordinaryClosure.drainUs).Str(),
         Pretty(ordinaryClosure.reconcileUs).Str(),
         Pretty(stages.finalizerClassifyUs).Str(),
         Pretty(stages.resurrectionPublishUs).Str(),
         Pretty(resurrectionClosure.drainUs).Str(),
         Pretty(stages.weakRefUs).Str(),
         Pretty(stages.verifyUs).Str(),
         Pretty(stages.idleTransitionUs).Str(),
         Pretty(stages.sweepPlanUs).Str(),
         Pretty(stages.stwResumeUs).Str(),
         Pretty(otherUs).Str(),
         Pretty(stages.stwBodyUs).Str(),
         visitedBarrierBuffers, nonEmptyBarrierBuffers,
         bufferedBarrierEntries);
    VLOG(REPORT,
         "[MS-GC] MarkTermination total: %sus ordinaryWU=%llu "
         "resurrectionRoots=%zu resurrectionPublishWU=%llu "
         "resurrectionClosureWU=%llu",
         Pretty(cycleStats_.markTerminationUs).Str(),
         static_cast<unsigned long long>(ordinaryClosure.workUnits),
         resurrectionEntries,
         static_cast<unsigned long long>(
             resurrectionPublishWork.Total()),
         static_cast<unsigned long long>(resurrectionClosure.workUnits));
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


void MarkSweepCollector::ClearWeakRefsAfterMark(
    size_t& cleared, size_t& preserved)
{
    cleared = 0;
    preserved = 0;
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
}

// ============================================================================
// Step 2.4: PostMarkTermination (non-STW, concurrent-safe)
// ============================================================================

void MarkSweepCollector::PostMarkTermination()
{
    uint64_t phaseStart = TimeUtil::MicroSeconds();
    FinalizeConcurrentMarkAccounting();
    VLOG(REPORT, "[MS-GC] --- PostMarkTermination ---");

    // Phase transition and WB clear already done in MarkTermination (inside STW).
    // No need to transition again or clear WB buffers here.

    // Finalizer reachability, resurrection and publication completed under
    // STW. CollectorResources::NotifyGCFinished wakes the background processor
    // after its GC-active gate opens; notifying it here would only wake it too
    // early and then require the same notification again.

    // Publish stable mark statistics and hand Small Region work to background
    // Sweep. MarkTermination already restored IDLE and installed the idle
    // barrier before mutators resumed, so this does not change barrier
    // semantics.
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(theAllocator);
    CentralRegionPool* centralPool = space.GetCentralPool();
    MSPostMarkSweepPlan sweepPlan = BuildMSPostMarkSweepPlan(gcReason == GC_REASON_OOM);
    if (centralPool != nullptr) {
        GCThreadPool* threadPool = GetThreadPool();
        size_t availableHelpers = threadPool == nullptr ? 0 : std::min(
            collectorResources.GetMSGCThreadPoolHelperLimit(),
            static_cast<size_t>(threadPool->GetMaxThreadNum()));
        size_t statsHelperCount = ComputeMSPostMarkStatsHelperTarget(
            centralPool->GetCollectSetBitmapBytes(), availableHelpers);
        bool statsPoolOwned = statsHelperCount != 0 &&
            collectorResources.AcquireMSGCWorkerPoolForPostMarkStats();
        if (!statsPoolOwned) {
            statsHelperCount = 0;
        }

        MSMarkedStats markedStats = centralPool->ComputeMarkedStats(
            threadPool, statsHelperCount);
        if (statsPoolOwned) {
            collectorResources.ReleaseMSGCWorkerPool(
                MSGCWorkerPoolOwner::POST_MARK_STATS);
        }
        cycleStats_.trueLiveBytes = markedStats.liveBytes;
        prevCycleMarkedObjects_.store(markedStats.objectCount, std::memory_order_relaxed);
        VLOG(REPORT, "[MS-GC] Mark bitmap: objects=%zu liveBytes=%zuB",
             markedStats.objectCount, markedStats.liveBytes);

        // Compute the next-cycle allocation base while LARGE next-supply still
        // contains only objects allocated after the frozen PreEnum boundary.
        // CollectLargeGarbage() appends frozen LARGE survivors to that vector,
        // so the outside LARGE span snapshot must precede it.
        MSGCThresholdBaseDecision thresholdBase = ComputeMSGCThresholdBase(
            markedStats.liveBytes,
            markReadyProcessorCount_,
            kNumSizeClasses * kMSScanClassCount,
            MS_REGION_SIZE,
            centralPool->GetSmallSweptRegionCount(),
            centralPool->GetLargeNextSupplySpanCount());
        cycleStats_.thresholdBaseBytes = thresholdBase.gcBaseBytes;
        cycleStats_.tlsReservationBytes = thresholdBase.tlsReservationBytes;
        cycleStats_.outsideCentralSmallBytes = thresholdBase.outsideCentralSmallBytes;
        cycleStats_.outsideLargeBytes = thresholdBase.outsideLargeBytes;

        // Partition the frozen LARGE collect-set only after statistics have
        // consumed it. Survivors are appended to largeNextSupply_; dead spans
        // are reset and returned directly to FreeManager.
        CollectLargeGarbage();

        // Sweep Assist derives its runway from Heap::GetHeapThresholdBytes().
        // Install this cycle's live-based threshold before publishing the new
        // Sweep epoch so its initial assist ratio never observes the old goal.
        UpdateGCStats();

        // The immutable Mark boundary becomes Sweep work only after the final
        // bitmap/stats consumers have finished. Regions returned or allocated
        // after PreEnum remain in swept allocation supply for the next cycle.
        centralPool->PublishCollectSetForSweep();

        if (sweepPlan.parallelSweepOnceInPostMark) {
            RunOOMEmergencyParallelSweep(*centralPool);
        }
        if (sweepPlan.flushAllCaches) {
            size_t flushedEmptyBytes = space.FlushAllEmptySweptToHotFree();
            VLOG(REPORT,
                 "[MS-GC] OOM cache flush: empty=%zuB hotFree=%zuB",
                 flushedEmptyBytes, space.GetHotFreeBytesReference());
            centralPool->ReenableSweepAdmission();
        }
    } else {
        // Preserve collector accounting even if the allocator has no central
        // pool (initialization/teardown defensive path).
        UpdateGCStats();
    }

    BackgroundProcessor& backgroundProcessor = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
    if (sweepPlan.notifyBackgroundSweep) {
        backgroundProcessor.NotifyToSweep(kNoPreferredSweepClass);
    }
    cycleStats_.postMarkUs = TimeUtil::MicroSeconds() - phaseStart;
    VLOG(REPORT, "[MS-GC] PostMark total: %sus",
         Pretty(cycleStats_.postMarkUs).Str());
}

void MarkSweepCollector::RunOOMEmergencyParallelSweep(CentralRegionPool& centralPool)
{
    size_t unsweptBefore = centralPool.GetTotalUnsweptCount();
    size_t configuredThreads = std::min(
        static_cast<size_t>(std::max<int32_t>(1, CangjieRuntime::GetGCParam().gcThreads)),
        CangjieRuntime::GetMSGCThreadCapacity());
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
    VLOG(REPORT, "[MS-GC] unswept: %zu regions, emptyCache: %zu regions, hotFree: %zu regions",
        unsweptCount, emptyCount, hotFreeCount);
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
    size_t previousInUseBytes = gcStats.lastRegionBytes;
    // ComputeMarkedStats supplies exact live bytes for the frozen collect-set.
    // PostMarkTermination has already combined that value with the fixed TLS
    // reservation and Region-capacity snapshots for both outside-set supplies.
    size_t liveBytes = cycleStats_.trueLiveBytes;
    size_t inUseBytes = space.GetHeapInUseBytes();
    size_t maxHeapBytes = space.GetMaxCapacity();
    cycleStats_.markEndInUseBytes = inUseBytes;
    cycleStats_.inUseBurstBytes =
        inUseBytes > cycleStats_.markStartInUseBytes
        ? inUseBytes - cycleStats_.markStartInUseBytes
        : 0;
    bool exceptionalBypass = gcStats.reason == GC_REASON_OOM;
    cycleStats_.floatingLiveInflated = false;
    gcStats.lastStableLiveBytes = liveBytes;
    gcStats.lastTrueLiveBytes = liveBytes;
    gcStats.lastRegionBytes = inUseBytes;
    gcStats.baselineAllocatedBytes = inUseBytes;

    // The raw MSGC target is the outside-set-aware base * (1 + heapGrowth).
    // Established normal cycles use an inclusive 10% deadband and symmetric
    // 12.5% transition limits. Bootstrap and OOM cycles establish their
    // clamped target directly.
    size_t baseBytes = cycleStats_.thresholdBaseBytes;
    if (baseBytes == 0) {
        MSGCThresholdBaseDecision thresholdBase = ComputeMSGCThresholdBase(
            liveBytes,
            markReadyProcessorCount_,
            kNumSizeClasses * kMSScanClassCount,
            MS_REGION_SIZE,
            0,
            0);
        baseBytes = thresholdBase.gcBaseBytes;
        cycleStats_.thresholdBaseBytes = thresholdBase.gcBaseBytes;
        cycleStats_.tlsReservationBytes = thresholdBase.tlsReservationBytes;
    }

    double heapGrowth = CangjieRuntime::GetHeapParam().heapGrowth;
    size_t gcThresholdCap = maxHeapBytes != 0
        ? maxHeapBytes
        : CangjieRuntime::GetGCParam().gcThreshold;
    size_t absoluteMinThreshold =
        std::min(gcThresholdCap, static_cast<size_t>(20) * 1024 * 1024);
    size_t minThreshold = ComputeMSHeapThresholdFloor(
        absoluteMinThreshold, gcThresholdCap);
    bool bypassTransition =
        exceptionalBypass || oldLastTrueLiveBytes == 0;
    MSHeapThresholdDecision thresholdDecision =
        EvaluateMSHeapThreshold(
            oldThreshold, baseBytes, heapGrowth, minThreshold, gcThresholdCap,
            bypassTransition);
    size_t newThreshold = thresholdDecision.threshold;
    double scavengeGrowth = CangjieRuntime::GetGCParam().scavengeGrowth;
    MSScavengeThresholdDecision scavengeDecision = EvaluateMSScavengeThreshold(
        newThreshold,
        cycleStats_.outsideCentralSmallBytes,
        cycleStats_.outsideLargeBytes,
        maxHeapBytes,
        scavengeGrowth);
    size_t newScavengeThreshold = scavengeDecision.thresholdBytes;
    uint64_t completedWU =
        cycleCompletedMarkWU_.load(std::memory_order_relaxed);
    uint64_t allocatedMarkBytes =
        cycleAllocatedMarkBytes_.load(std::memory_order_relaxed);
    // Publish the physical retained target before the release-store of the GC
    // trigger threshold. Region acquisition reads the GC threshold first.
    gcStats.SetScavengeThreshold(newScavengeThreshold);
    gcStats.SetThreshold(newThreshold);
    gcStats.lastSurvivingRegionBytes = liveBytes;
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
         "[MS-GC] heapThreshold: old=%zuB new=%zuB prevLive=%zuB trueLive=%zuB "
         "tlsReserve=%zuB outsideSmall=%zuB outsideLarge=%zuB base=%zuB "
         "growth=%.2f minThreshold=%zuB maxHeap=%zuB",
         oldThreshold, newThreshold, oldLastTrueLiveBytes, liveBytes,
         cycleStats_.tlsReservationBytes,
         cycleStats_.outsideCentralSmallBytes,
         cycleStats_.outsideLargeBytes,
         baseBytes, 1.0 + heapGrowth, minThreshold, maxHeapBytes);
    VLOG(REPORT,
         "[MS-GC] heapThreshold stability: rawTarget=%zuB deadbandLower=%zuB "
         "deadbandUpper=%zuB deadbandHeld=%d fallLimit=%zuB riseLimit=%zuB "
         "riseLimited=%d fallLimited=%d transitionLimited=%d transitionBypassed=%d "
         "exceptionalBypass=%d previousInUse=%zuB inUse=%zuB markStart=%zuB markEnd=%zuB burst=%zuB",
         thresholdDecision.rawTarget,
         thresholdDecision.deadbandLower, thresholdDecision.deadbandUpper,
         static_cast<int>(thresholdDecision.deadbandHeld),
         thresholdDecision.fallLimit, thresholdDecision.riseLimit,
         static_cast<int>(thresholdDecision.riseLimited),
         static_cast<int>(thresholdDecision.fallLimited),
         static_cast<int>(thresholdDecision.transitionLimited),
         static_cast<int>(thresholdDecision.transitionBypassed),
         static_cast<int>(exceptionalBypass), previousInUseBytes, inUseBytes,
         cycleStats_.markStartInUseBytes, cycleStats_.markEndInUseBytes,
         cycleStats_.inUseBurstBytes);
    VLOG(REPORT,
         "[MS-GC] scavengeThreshold: old=%zuB new=%zuB gcGoal=%zuB "
         "outsideSmall=%zuB outsideLarge=%zuB base=%zuB growth=%.2f",
         oldScavengeThreshold, newScavengeThreshold,
         scavengeDecision.gcGoalBytes,
         scavengeDecision.outsideCentralSmallBytes,
         scavengeDecision.outsideLargeBytes,
         scavengeDecision.baseBytes, scavengeGrowth);

    VLOG(REPORT,
         "[MS-GC] live bytes %zu, inUseRegionBytes %zu, update gc threshold %zu -> %zu",
         liveBytes, inUseBytes, oldThreshold, gcStats.GetThreshold());
    uint64_t updateStatsUs = TimeUtil::MicroSeconds() - updateStatsStart;
    VLOG(REPORT, "[MS-GC] UpdateGCStats took %lu us", static_cast<unsigned long>(updateStatsUs));
    TRACE_COUNT("CJRT_post_GC_HeapSize", Heap::GetHeap().GetAllocatedSize());
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
