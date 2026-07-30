// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MSMarkBarrier.h"
#include "Heap/Barrier/Barrier.inline.h"
#include "Heap/Heap.h"
#include "Heap/MBarrier/MSRefField.inline.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MCollector/MarkSweepCollector.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include <cstdlib>
#include <cstring>

namespace MapleRuntime {
static inline bool IsMSGCMarkPhase(GCPhase phase)
{
    switch (phase) {
        case GCPhase::GC_PHASE_MS_MARK_SETUP:
        case GCPhase::GC_PHASE_MS_CONCURRENT_MARK:
        case GCPhase::GC_PHASE_MS_MARK_TERMINATION:
            return true;
        default:
            return false;
    }
}

static inline MarkSweepCollector* ResolveMarkOwner()
{
    GCPhase phase = static_cast<GCPhase>(
        ThreadLocal::GetThreadLocalData()->encodedGCPhase);
    if (!IsMSGCMarkPhase(phase)) {
        return nullptr;
    }
    return MarkSweepCollector::GetActiveCollector();
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
    BarrierMarkContext()
    {
        collector_ = ResolveMarkOwner();
        if (collector_ != nullptr) {
            allocBuffer_ = AllocBufferMS::GetAllocBufferMS();
            if (allocBuffer_ != nullptr) {
                buffer_ = &allocBuffer_->GetBarrierMarkBuffer();
            }
        }
    }

    BarrierMarkContext(const BarrierMarkContext&) = delete;
    BarrierMarkContext& operator=(const BarrierMarkContext&) = delete;

    bool IsActive() const
    {
        return collector_ != nullptr;
    }

    void AddOldRef(BaseObject* ref)
    {
        // Check the phase/collector gate first. Runtime fallback calls may
        // still reach this helper during IDLE, where even a heap-range check
        // is unnecessary.
        if (UNLIKELY(collector_ == nullptr || ref == nullptr ||
                     !Heap::IsHeapAddress(ref))) {
            return;
        }
        if (LIKELY(buffer_ != nullptr)) {
            collector_->AddManagedRefToBarrierBuffer(ref, *buffer_);
        } else {
            collector_->PublishManagedRefForBarrierImmediately(ref);
        }
    }

private:
    MarkSweepCollector* collector_ = nullptr;
    AllocBufferMS* allocBuffer_ = nullptr;
    MSBarrierMarkBuffer* buffer_ = nullptr;
};

static inline void EnqueueStructRefsInRange(
    BarrierMarkContext& marker, BaseObject* obj, MAddress start, size_t size)
{
    if (!marker.IsActive() || obj == nullptr ||
        obj->GetTypeInfo() == nullptr || size == 0) {
        return;
    }
    obj->ForEachRefInStruct(
        [&marker](RefField<false>& refField) {
            marker.AddOldRef(MSLoadRawReference(refField));
        },
        start, start + size);
}

static inline void EnqueueStructArrayRefsInRange(
    BarrierMarkContext& marker, BaseObject* obj, MAddress start, size_t size)
{
    if (!marker.IsActive() || obj == nullptr ||
        obj->GetTypeInfo() == nullptr || size == 0 ||
        !obj->HasRefField()) {
        return;
    }
    if (!obj->IsRawArray()) {
        EnqueueStructRefsInRange(marker, obj, start, size);
        return;
    }

    MArray* array = static_cast<MArray*>(obj);
    TypeInfo* component = array->GetComponentTypeInfo();
    if (component == nullptr || !component->IsStructType() || !component->HasRefField()) {
        return;
    }
    array->ForEachRefFieldInRange(
        [&marker](RefField<false>& refField) {
            marker.AddOldRef(MSLoadRawReference(refField));
        },
        start, start + size);
}

static inline void EnqueueStaticStructRefs(
    BarrierMarkContext& marker, MAddress start, const GCTib gctib)
{
    if (!marker.IsActive()) {
        return;
    }
    gctib.ForEachBitmapWord(start, [&marker](RefField<>& refField) {
        marker.AddOldRef(MSLoadRawReference(refField));
    });
}

static inline void EnqueueRefArrayRange(
    BarrierMarkContext& marker, MAddress start, size_t size)
{
    if (!marker.IsActive() || size == 0) {
        return;
    }
    size_t refCount = size / sizeof(MAddress);
    RefField<false>* refs = reinterpret_cast<RefField<false>*>(start);
    for (size_t i = 0; i < refCount; ++i) {
        marker.AddOldRef(MSLoadRawReference(refs[i]));
    }
}

// ============================================================================
// Writes: deletion-style barrier — log(old) + store
// ============================================================================

static inline void WriteRefField(RefField<false>& field, BaseObject* ref)
{
    BaseObject* oldRef = MSLoadRawReference(field);
    if (oldRef != ref) {
        BarrierMarkContext marker;
        marker.AddOldRef(oldRef);
    }
    MSStoreRawReference(field, ref);
}

void MSBarrierOps::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref)
{
    (void)obj;
    WriteRefField(field, ref);
}

void MSBarrierOps::WriteStaticRef(RefField<false>& field, BaseObject* ref)
{
    WriteRefField(field, ref);
}

void MSBarrierOps::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen)
{
    // log(old): preserve all existing reference fields in the destination struct
    size_t copiedLen = (dstLen < srcLen ? dstLen : srcLen);
    BarrierMarkContext marker;
    EnqueueStructRefsInRange(marker, obj, dst, copiedLen);

    // memcpy (overwrites dst with src)
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                          reinterpret_cast<void*>(src), srcLen) == EOK,
                 "MSMarkBarrier WriteStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    CHECK_EQ(srcLen, dstLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

void MSBarrierOps::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                     const GCTib gctib)
{
    // log(old): preserve all existing reference fields via GCTib bitmap
    BarrierMarkContext marker;
    EnqueueStaticStructRefs(marker, dst, gctib);

    // memcpy
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                          reinterpret_cast<void*>(src), srcLen) == EOK,
                 "MSMarkBarrier WriteStaticStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstLen < srcLen ? dstLen : srcLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), copyLen);
#endif
}

// ============================================================================
// Array copy: discover overwritten refs before they are replaced
// ============================================================================

void MSBarrierOps::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                BaseObject* srcObj, MAddress srcField, MIndex srcSize)
{
    (void)dstObj;
    (void)srcObj;

    // discover(old): preserve all existing references in the destination range
    size_t copiedLen = (dstSize < srcSize ? dstSize : srcSize);
    BarrierMarkContext marker;
    EnqueueRefArrayRange(marker, dstField, copiedLen);

    // memmove
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                           reinterpret_cast<void*>(srcField), srcSize) == EOK,
                 "MSMarkBarrier CopyRefArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
}

void MSBarrierOps::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                   BaseObject* srcObj, MAddress srcField, MIndex srcSize)
{
    (void)srcObj;
    size_t copiedLen = (dstSize < srcSize ? dstSize : srcSize);

    // discover(old): preserve all existing reference fields in the destination range
    BarrierMarkContext marker;
    EnqueueStructArrayRefsInRange(marker, dstObj, dstField, copiedLen);

    // memmove
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                           reinterpret_cast<void*>(srcField), srcSize) == EOK,
                 "MSMarkBarrier CopyStructArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
}

void MSMarkBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    MSBarrierOps::WriteReference(obj, field, ref);
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
    MSBarrierOps::CopyRefArray(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
}

void MSMarkBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                    BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    MSBarrierOps::CopyStructArray(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
}

// ============================================================================
// Atomic operations: discover(old)
// ============================================================================

void MSMarkBarrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field,
                                         BaseObject* ref, MemoryOrder order) const
{
    (void)obj;
    MemoryOrder failOrder = CASFailureOrder(order);
    BaseObject* expected =
        MSLoadRawReference(field, std::memory_order_relaxed);
    BarrierMarkContext marker;
    for (;;) {
        // Capture completes before the CAS can delete this exact edge. The
        // owner-private bitmap mask may remain pending after this wrapper.
        marker.AddOldRef(expected);
        if (MSCompareExchangeRawReference(
                field, expected, ref, order, failOrder)) {
            return;
        }
        expected = MSLoadRawReference(field, failOrder);
    }
}

BaseObject* MSMarkBarrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field,
                                               BaseObject* newRef, MemoryOrder order) const
{
    (void)obj;

    MemoryOrder failOrder = CASFailureOrder(order);
    BaseObject* expected =
        MSLoadRawReference(field, std::memory_order_relaxed);
    BarrierMarkContext marker;
    for (;;) {
        marker.AddOldRef(expected);
        if (MSCompareExchangeRawReference(
                field, expected, newRef, order, failOrder)) {
            return expected;
        }
        expected = MSLoadRawReference(field, failOrder);
    }
}

bool MSMarkBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                            BaseObject* oldRef, BaseObject* newRef,
                                            MemoryOrder succOrder, MemoryOrder failOrder) const
{
    (void)obj;

    // Conservatively shade the expected edge before the mutation. A failed CAS
    // may perform redundant work, but a successful CAS can never expose an
    // unprotected deletion window.
    BarrierMarkContext marker;
    marker.AddOldRef(oldRef);
    return MSCompareExchangeRawReference(
        field, oldRef, newRef, succOrder, failOrder);
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
