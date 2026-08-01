// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MSMarkBarrier.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "Heap/Heap.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MCollector/MarkSweepCollector.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/MArray.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
static constexpr size_t kMSBarrierRuntimeStatCount =
    static_cast<size_t>(MSBarrierRuntimeStat::COUNT);
static std::atomic<uint64_t>
    g_msBarrierRuntimeStats[kMSBarrierRuntimeStatCount];

static const char* GetMSBarrierRuntimeStatName(MSBarrierRuntimeStat stat)
{
    static constexpr const char* names[] = {
        "old_ref_buffer_request",
        "old_ref_resolve_rejected",
        "old_ref_large",
        "old_ref_already_pending",
        "old_ref_added_without_flush",
        "old_ref_added_without_flush_collect_set",
        "old_ref_added_without_flush_outside_set",
        "old_ref_threshold_flush",
        "old_ref_set_conflict_flush",
        "old_ref_immediate_publish",
    };
    static_assert(
        sizeof(names) / sizeof(names[0]) == kMSBarrierRuntimeStatCount,
        "MSGC barrier stat names are out of sync");
    return names[static_cast<size_t>(stat)];
}

static void DumpMSBarrierRuntimeStats()
{
    std::fprintf(stderr, "[MSGC_BARRIER_STATS] begin\n");
    for (size_t i = 0; i < kMSBarrierRuntimeStatCount; ++i) {
        std::fprintf(
            stderr, "[MSGC_BARRIER_STATS] %s=%llu\n",
            GetMSBarrierRuntimeStatName(
                static_cast<MSBarrierRuntimeStat>(i)),
            static_cast<unsigned long long>(
                g_msBarrierRuntimeStats[i].load(std::memory_order_relaxed)));
    }
    std::fprintf(stderr, "[MSGC_BARRIER_STATS] end\n");
}

static bool IsMSBarrierRuntimeStatsEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("CJ_MSGC_BARRIER_COUNT");
        bool on = value != nullptr && value[0] != '\0' &&
            !(value[0] == '0' && value[1] == '\0');
        if (on) {
            std::atexit(DumpMSBarrierRuntimeStats);
        }
        return on;
    }();
    return enabled;
}

void RecordMSBarrierRuntimeStat(MSBarrierRuntimeStat stat)
{
    if (UNLIKELY(IsMSBarrierRuntimeStatsEnabled())) {
        g_msBarrierRuntimeStats[static_cast<size_t>(stat)].fetch_add(
            1, std::memory_order_relaxed);
    }
}

static inline bool IsMSGCMarkPhase(GCPhase phase)
{
    static_assert(
        static_cast<uint8_t>(GCPhase::GC_PHASE_MS_CONCURRENT_MARK) ==
            static_cast<uint8_t>(GCPhase::GC_PHASE_MS_MARK_SETUP) + 1 &&
        static_cast<uint8_t>(GCPhase::GC_PHASE_MS_MARK_TERMINATION) ==
            static_cast<uint8_t>(GCPhase::GC_PHASE_MS_CONCURRENT_MARK) + 1,
        "MSGC mark GCPhase range must remain contiguous");
    return phase >= GCPhase::GC_PHASE_MS_MARK_SETUP &&
        phase <= GCPhase::GC_PHASE_MS_MARK_TERMINATION;
}

static inline MemoryOrder CASFailureOrder(MemoryOrder order)
{
    if (order == std::memory_order_release) {
        return std::memory_order_relaxed;
    }
    if (order == std::memory_order_acq_rel) {
        return std::memory_order_acquire;
    }
    return order;
}

class BarrierMarkContext {
public:
    BarrierMarkContext() : markActive_(IsMSGCMarkPhase(static_cast<GCPhase>(
        ThreadLocal::GetThreadLocalData()->encodedGCPhase))) {}

    BarrierMarkContext(const BarrierMarkContext&) = delete;
    BarrierMarkContext& operator=(const BarrierMarkContext&) = delete;

    bool IsActive() const
    {
        return markActive_;
    }

    void AddOldRefActive(BaseObject* ref)
    {
        if (UNLIKELY(ref == nullptr || !Heap::IsHeapAddress(ref))) {
            return;
        }
        EnsureOwner();
        if (UNLIKELY(collector_ == nullptr)) {
            return;
        }
        if (LIKELY(buffer_ != nullptr)) {
            collector_->AddManagedRefToBarrierBuffer(
                ref, *buffer_, currentMarkEpoch_, allocPlaneBase_,
                markPlaneBase_);
        } else {
            RecordMSBarrierRuntimeStat(
                MSBarrierRuntimeStat::OLD_REF_IMMEDIATE_PUBLISH);
            collector_->PublishManagedRefForBarrierImmediately(ref);
        }
    }

private:
    void EnsureOwner()
    {
        if (LIKELY(ownerResolved_)) {
            return;
        }
        ownerResolved_ = true;
        collector_ = MarkSweepCollector::GetActiveCollector();
        if (UNLIKELY(collector_ == nullptr)) {
            return;
        }
        AllocBufferMS* allocBuffer = AllocBufferMS::GetAllocBufferMS();
        if (allocBuffer != nullptr) {
            buffer_ = &allocBuffer->GetBarrierMarkBuffer();
            currentMarkEpoch_ = __atomic_load_n(
                &CJ_MRT_MSGCFastPathState.currentMarkEpoch,
                __ATOMIC_ACQUIRE);
            allocPlaneBase_ = static_cast<uintptr_t>(__atomic_load_n(
                &CJ_MRT_MSGCFastPathState.allocPlaneBase,
                __ATOMIC_ACQUIRE));
            markPlaneBase_ = static_cast<uintptr_t>(__atomic_load_n(
                &CJ_MRT_MSGCFastPathState.markPlaneBase,
                __ATOMIC_ACQUIRE));
        }
    }

    MarkSweepCollector* collector_ = nullptr;
    MSBarrierMarkBuffer* buffer_ = nullptr;
    uint32_t currentMarkEpoch_ = MSRegionInfo::NO_COLLECT_EPOCH;
    uintptr_t allocPlaneBase_ = 0;
    uintptr_t markPlaneBase_ = 0;
    bool markActive_ = false;
    bool ownerResolved_ = false;
};

class BulkOldRefVisitor {
public:
    explicit BulkOldRefVisitor(BarrierMarkContext& marker) : marker_(marker) {}

    void operator()(RefField<>& refField)
    {
        BaseObject* oldRef = MSLoadRawReference(refField);
        if (LIKELY(oldRef == lastOldRef_)) {
            return;
        }
        lastOldRef_ = oldRef;
        marker_.AddOldRefActive(oldRef);
    }

private:
    BarrierMarkContext& marker_;
    BaseObject* lastOldRef_ = nullptr;
};

static inline void EnqueueGCTibRefsInRange(
    BarrierMarkContext& marker, MAddress base, MAddress start, size_t size,
    const GCTib gctib)
{
    BulkOldRefVisitor visitOldRef(marker);
    gctib.ForEachBitmapWordInRangeFast(
        base, visitOldRef, start, start + size);
}

static inline void EnqueueStructRefsInRange(
    BarrierMarkContext& marker, BaseObject* obj, MAddress start, size_t size)
{
    if (!marker.IsActive() || obj == nullptr) {
        return;
    }
    TypeInfo* typeInfo = obj->GetTypeInfo();
    if (typeInfo == nullptr) {
        return;
    }
    if (LIKELY(!typeInfo->IsRawArray())) {
        if (!typeInfo->HasRefField()) {
            return;
        }
        EnqueueGCTibRefsInRange(
            marker, reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE,
            start, size, typeInfo->GetGCTib());
        return;
    }

    MArray* array = static_cast<MArray*>(obj);
    TypeInfo* component = typeInfo->GetComponentTypeInfo();
    if (UNLIKELY(component == nullptr || !component->IsStructType() ||
                 !component->HasRefField())) {
        return;
    }
    size_t elementSize = component->GetComponentSize();
    CHECK_DETAIL(elementSize != 0,
                 "MSMarkBarrier RawArray has a zero-sized component");
    MAddress content =
        reinterpret_cast<MAddress>(array) + MArray::GetContentOffset();
    MIndex startIndex =
        static_cast<MIndex>((start - content) / elementSize);
    MAddress current = content + startIndex * elementSize;
    MAddress end = start + size;
    MRT_ASSERT(current + elementSize >= end,
               "aggregate element is not aligned");

    GCTib gctib = component->GetGCTib();
    BulkOldRefVisitor visitOldRef(marker);
    MIndex length = array->GetLength();
    if (startIndex < length && current < end) {
        gctib.ForEachBitmapWordFast(current, visitOldRef);
    }
}

static inline void EnqueueStructArrayRefsInRange(
    BarrierMarkContext& marker, BaseObject* obj, MAddress start, size_t size)
{
    if (!marker.IsActive() || obj == nullptr) {
        return;
    }
    TypeInfo* typeInfo = obj->GetTypeInfo();
    if (typeInfo == nullptr) {
        return;
    }
    if (LIKELY(!typeInfo->IsRawArray())) {
        if (!typeInfo->HasRefField()) {
            return;
        }
        EnqueueGCTibRefsInRange(
            marker, reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE,
            start, size, typeInfo->GetGCTib());
        return;
    }

    TypeInfo* component = typeInfo->GetComponentTypeInfo();
    if (component == nullptr || !component->IsStructType() || !component->HasRefField()) {
        return;
    }
    size_t elementSize = component->GetComponentSize();
    CHECK_DETAIL(elementSize != 0,
                 "MSMarkBarrier RawArray has a zero-sized component");
    MRT_ASSERT(size % elementSize == 0,
               "MSMarkBarrier RawArray copy is not element aligned");
    GCTib gctib = component->GetGCTib();
    BulkOldRefVisitor visitOldRef(marker);
    MAddress current = start;
    MAddress end = start + size;
    while (current < end) {
        gctib.ForEachBitmapWordFast(current, visitOldRef);
        current += elementSize;
    }
}

static inline void EnqueueStaticStructRefs(
    BarrierMarkContext& marker, MAddress start, size_t size, const GCTib gctib)
{
    if (!marker.IsActive()) {
        return;
    }
    EnqueueGCTibRefsInRange(marker, start, start, size, gctib);
}

static inline void EnqueueRefArrayRange(
    BarrierMarkContext& marker, MAddress start, size_t size)
{
    if (!marker.IsActive()) {
        return;
    }
    size_t refCount = size / sizeof(MAddress);
    RefField<false>* refs = reinterpret_cast<RefField<false>*>(start);
    BulkOldRefVisitor visitOldRef(marker);
    for (size_t i = 0; i < refCount; ++i) {
        visitOldRef(refs[i]);
    }
}

// ============================================================================
// Writes: deletion-style barrier — discover(old) + store
// ============================================================================

static inline void WriteRefField(RefField<false>& field, BaseObject* ref)
{
    BarrierMarkContext marker;
    if (LIKELY(marker.IsActive())) {
        BaseObject* oldRef = MSLoadRawReference(field);
        if (oldRef != ref) {
            marker.AddOldRefActive(oldRef);
        }
    }
    MSStoreRawReference(field, ref);
}

void MSBarrierOps::WriteReference(RefField<false>& field, BaseObject* ref)
{
    WriteRefField(field, ref);
}

void MSBarrierOps::WriteStaticRef(RefField<false>& field, BaseObject* ref)
{
    WriteRefField(field, ref);
}

void MSBarrierOps::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen)
{
    if (srcLen == 0) {
        return;
    }
    CHECK_DETAIL(srcLen <= dstLen,
                 "MSMarkBarrier WriteStruct source exceeds destination");
    CHECK_DETAIL(dst != 0 && src != 0,
                 "MSMarkBarrier WriteStruct received a null address");

    // discover(old): preserve exactly the destination range being overwritten
    BarrierMarkContext marker;
    EnqueueStructRefsInRange(marker, obj, dst, srcLen);

    // memcpy (overwrites dst with src)
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                          reinterpret_cast<void*>(src), srcLen) == EOK,
                 "MSMarkBarrier WriteStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), srcLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

void MSBarrierOps::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                     const GCTib gctib)
{
    if (srcLen == 0) {
        return;
    }
    CHECK_DETAIL(srcLen <= dstLen,
                 "MSMarkBarrier WriteStaticStruct source exceeds destination");
    CHECK_DETAIL(dst != 0 && src != 0,
                 "MSMarkBarrier WriteStaticStruct received a null address");

    // discover(old): restrict GCTib enumeration to the overwritten range
    BarrierMarkContext marker;
    EnqueueStaticStructRefs(marker, dst, srcLen, gctib);

    // memcpy
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                          reinterpret_cast<void*>(src), srcLen) == EOK,
                 "MSMarkBarrier WriteStaticStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), srcLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

// ============================================================================
// Array copy: discover overwritten refs before they are replaced
// ============================================================================

void MSBarrierOps::CopyRefArray(MAddress dstField, MIndex dstSize,
                                MAddress srcField, MIndex srcSize)
{
    if (srcSize == 0) {
        return;
    }
    CHECK_DETAIL(srcSize <= dstSize,
                 "MSMarkBarrier CopyRefArray source exceeds destination");
    MRT_ASSERT(dstSize <= SECUREC_MEM_MAX_LEN,
               "size too big in CJ_MCC_ArrayCopy");
    CHECK_DETAIL(dstField != 0 && srcField != 0,
                 "MSMarkBarrier CopyRefArray received a null address");

    // discover(old): preserve all existing references in the destination range
    BarrierMarkContext marker;
    EnqueueRefArrayRange(marker, dstField, srcSize);

    // memmove
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                           reinterpret_cast<void*>(srcField), srcSize) == EOK,
                 "MSMarkBarrier CopyRefArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), srcSize);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
}

void MSBarrierOps::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                   MAddress srcField, MIndex srcSize)
{
    if (srcSize == 0) {
        return;
    }
    CHECK_DETAIL(srcSize <= dstSize,
                 "MSMarkBarrier CopyStructArray source exceeds destination");
    MRT_ASSERT(dstSize <= SECUREC_MEM_MAX_LEN,
               "size too big in CJ_MCC_ArrayCopy");
    CHECK_DETAIL(dstField != 0 && srcField != 0,
                 "MSMarkBarrier CopyStructArray received a null address");

    // discover(old): preserve all existing reference fields in the destination range
    BarrierMarkContext marker;
    EnqueueStructArrayRefsInRange(marker, dstObj, dstField, srcSize);

    // memmove
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                           reinterpret_cast<void*>(srcField), srcSize) == EOK,
                 "MSMarkBarrier CopyStructArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), srcSize);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), srcSize);
#endif
}

void MSMarkBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    (void)obj;
    MSBarrierOps::WriteReference(field, ref);
}

void MSMarkBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const
{
    MSBarrierOps::WriteStaticRef(field, ref);
}

void MSMarkBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    MSBarrierOps::WriteStruct(obj, dst, dstLen, src, srcLen);
}

void MSMarkBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                      const GCTib gctib) const
{
    MSBarrierOps::WriteStaticStruct(dst, dstLen, src, srcLen, gctib);
}

void MSMarkBarrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                 BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    (void)dstObj;
    (void)srcObj;
    MSBarrierOps::CopyRefArray(dstField, dstSize, srcField, srcSize);
}

void MSMarkBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                    BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    (void)srcObj;
    MSBarrierOps::CopyStructArray(dstObj, dstField, dstSize, srcField, srcSize);
}

// ============================================================================
// Atomic operations: discover(old)
// ============================================================================

void MSBarrierOps::AtomicWriteReference(
    RefField<true>& field, BaseObject* ref, MemoryOrder order)
{
    BarrierMarkContext marker;
    if (LIKELY(!marker.IsActive())) {
        MSStoreRawReference(field, ref, order);
        return;
    }

    MemoryOrder failOrder = CASFailureOrder(order);
    BaseObject* expected =
        MSLoadRawReference(field, std::memory_order_relaxed);
    BaseObject* lastShaded = nullptr;
    bool hasLastShaded = false;
    for (;;) {
        // Capture completes before the CAS can delete this exact edge. The
        // owner-private bitmap mask may remain pending after this wrapper.
        if (expected != ref &&
            (!hasLastShaded || expected != lastShaded)) {
            marker.AddOldRefActive(expected);
            lastShaded = expected;
            hasLastShaded = true;
        }
        if (MSCompareExchangeRawReference(
                field, expected, ref, order, failOrder)) {
            return;
        }
        expected = MSLoadRawReference(field, failOrder);
    }
}

BaseObject* MSBarrierOps::AtomicSwapReference(
    RefField<true>& field, BaseObject* newRef, MemoryOrder order)
{
    BarrierMarkContext marker;
    if (LIKELY(!marker.IsActive())) {
        return MSExchangeRawReference(field, newRef, order);
    }

    MemoryOrder failOrder = CASFailureOrder(order);
    BaseObject* expected =
        MSLoadRawReference(field, std::memory_order_relaxed);
    BaseObject* lastShaded = nullptr;
    bool hasLastShaded = false;
    for (;;) {
        if (expected != newRef &&
            (!hasLastShaded || expected != lastShaded)) {
            marker.AddOldRefActive(expected);
            lastShaded = expected;
            hasLastShaded = true;
        }
        if (MSCompareExchangeRawReference(
                field, expected, newRef, order, failOrder)) {
            return expected;
        }
        expected = MSLoadRawReference(field, failOrder);
    }
}

bool MSBarrierOps::CompareAndSwapReference(
    RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
    MemoryOrder succOrder, MemoryOrder failOrder)
{
    if (oldRef == newRef) {
        return MSCompareExchangeRawReference(
            field, oldRef, newRef, succOrder, failOrder);
    }
    BarrierMarkContext marker;
    if (LIKELY(!marker.IsActive())) {
        return MSCompareExchangeRawReference(
            field, oldRef, newRef, succOrder, failOrder);
    }

    // Conservatively shade the expected edge before the mutation. A failed CAS
    // may perform redundant work, but a successful CAS can never expose an
    // unprotected deletion window.
    marker.AddOldRefActive(oldRef);
    return MSCompareExchangeRawReference(
        field, oldRef, newRef, succOrder, failOrder);
}

void MSMarkBarrier::AtomicWriteReference(
    BaseObject* obj, RefField<true>& field, BaseObject* ref,
    MemoryOrder order) const
{
    (void)obj;
    MSBarrierOps::AtomicWriteReference(field, ref, order);
}

BaseObject* MSMarkBarrier::AtomicSwapReference(
    BaseObject* obj, RefField<true>& field, BaseObject* newRef,
    MemoryOrder order) const
{
    (void)obj;
    return MSBarrierOps::AtomicSwapReference(field, newRef, order);
}

bool MSMarkBarrier::CompareAndSwapReference(
    BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
    BaseObject* newRef, MemoryOrder succOrder,
    MemoryOrder failOrder) const
{
    (void)obj;
    return MSBarrierOps::CompareAndSwapReference(
        field, oldRef, newRef, succOrder, failOrder);
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
