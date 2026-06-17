// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MSMarkBarrier.h"
#include "Heap/Barrier/Barrier.inline.h"
#include "Heap/Heap.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MCollector/MarkSweepCollector.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include <cstdlib>
#include <cstring>

namespace MapleRuntime {
namespace {
class WriteBarrierBufferGreySink final : public MarkSweepCollector::GreyWorkSink {
public:
    explicit WriteBarrierBufferGreySink(WriteBarrierBuffer& wbBuf) : wbBuf_(wbBuf) {}

    void PushGrey(BaseObject* obj) override
    {
        wbBuf_.Push(obj);
    }

private:
    WriteBarrierBuffer& wbBuf_;
};
} // namespace

static inline void EnqueueRefFast(BaseObject* ref)
{
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
        return;
    }
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) {
        MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
        if (collector == nullptr) {
            return;
        }
        WriteBarrierBuffer wbBuf;
        WriteBarrierBufferGreySink sink(wbBuf);
        (void)collector->TryEnqueueGreyObject(ref, sink);
        collector->FlushWriteBarrierOverflow(wbBuf);
        return;
    }
    WriteBarrierBuffer& wbBuf = mutator->GetWriteBarrierBuffer();
    if (UNLIKELY(wbBuf.IsFull())) {
        MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
        if (collector != nullptr) {
            collector->FlushWriteBarrierOverflow(wbBuf);
        } else {
            wbBuf.Clear();
        }
    }
    MarkSweepCollector& collector = static_cast<MarkSweepCollector&>(Heap::GetHeap().GetCollector());
    WriteBarrierBufferGreySink sink(wbBuf);
    (void)collector.TryEnqueueGreyObject(ref, sink);
}

static inline void EnqueueStructRefsInRange(BaseObject* obj, MAddress start, size_t size)
{
    if (obj == nullptr || obj->GetTypeInfo() == nullptr || size == 0) {
        return;
    }
    obj->ForEachRefInStruct(
        [](RefField<false>& refField) {
            EnqueueRefFast(refField.GetTargetObject());
        },
        start, start + size);
}

static inline void EnqueueStructArrayRefsInRange(BaseObject* obj, MAddress start, size_t size)
{
    if (obj == nullptr || obj->GetTypeInfo() == nullptr || size == 0 || !obj->HasRefField()) {
        return;
    }
    if (!obj->IsRawArray()) {
        EnqueueStructRefsInRange(obj, start, size);
        return;
    }

    MArray* array = static_cast<MArray*>(obj);
    TypeInfo* component = array->GetComponentTypeInfo();
    if (component == nullptr || !component->IsStructType() || !component->HasRefField()) {
        return;
    }
    array->ForEachRefFieldInRange(
        [](RefField<false>& refField) {
            EnqueueRefFast(refField.GetTargetObject());
        },
        start, start + size);
}

static inline void EnqueueStaticStructRefs(MAddress start, const GCTib gctib)
{
    gctib.ForEachBitmapWord(start, [](RefField<>& refField) {
        EnqueueRefFast(refField.GetTargetObject());
    });
}

static inline void EnqueueRefArrayRange(MAddress start, size_t size)
{
    size_t refCount = size / sizeof(MAddress);
    RefField<false>* refs = reinterpret_cast<RefField<false>*>(start);
    for (size_t i = 0; i < refCount; ++i) {
        EnqueueRefFast(refs[i].GetTargetObject());
    }
}

// ============================================================================
// Reads: zero-overhead direct load
// ============================================================================

BaseObject* MSMarkBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    (void)obj;
    return field.GetTargetObject();
}

BaseObject* MSMarkBarrier::ReadStaticRef(RefField<false>& field) const
{
    return field.GetTargetObject();
}

BaseObject* MSMarkBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    (void)obj;
    return field.GetTargetObject();
}

void MSMarkBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{
    (void)obj;
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size,
                          reinterpret_cast<void*>(src), size) == EOK,
                 "MSMarkBarrier ReadStruct memcpy_s failed");
}

void MSMarkBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    (void)gctib;
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size,
                          reinterpret_cast<void*>(src), size) == EOK,
                 "MSMarkBarrier ReadStaticStruct memcpy_s failed");
}

// ============================================================================
// Writes: deletion barrier only — discover(old) + store
// ============================================================================

void MSMarkBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    (void)obj;

    // discover(old)
    BaseObject* oldRef = field.GetTargetObject();
    EnqueueRefFast(oldRef);

    // store
    field.SetTargetObject(ref);
}

void MSMarkBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const
{
    // discover(old)
    BaseObject* oldRef = field.GetTargetObject();
    EnqueueRefFast(oldRef);

    // store
    field.SetTargetObject(ref);
}

void MSMarkBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    // discover(old): preserve all existing reference fields in the destination struct
    // Guard: skip shade if TypeInfo is NULL (prematurely reclaimed / uninitialized object)
    EnqueueStructRefsInRange(obj, dst, dstLen);

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

void MSMarkBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                      const GCTib gctib) const
{
    // discover(old): preserve all existing reference fields via GCTib bitmap
    EnqueueStaticStructRefs(dst, gctib);

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
// Array copy: discover overwritten refs only
// ============================================================================

void MSMarkBarrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                 BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    (void)dstObj;
    (void)srcObj;

    // discover(old): preserve all existing references in the destination range
    EnqueueRefArrayRange(dstField, dstSize);

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

void MSMarkBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                    BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    (void)srcObj;
    size_t copiedLen = (dstSize < srcSize ? dstSize : srcSize);

    // discover(old): preserve all existing reference fields in the destination range
    EnqueueStructArrayRefsInRange(dstObj, dstField, copiedLen);

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

// ============================================================================
// Atomic operations: discover(old) only
// ============================================================================

BaseObject* MSMarkBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field,
                                               MemoryOrder order) const
{
    (void)obj;
    RefField<false> tmpField(field.GetFieldValue(order));
    return tmpField.GetTargetObject();
}

void MSMarkBarrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field,
                                         BaseObject* ref, MemoryOrder order) const
{
    (void)obj;

    // discover(old)
    RefField<false> oldField(field.GetFieldValue(std::memory_order_relaxed));
    BaseObject* oldRef = oldField.GetTargetObject();
    EnqueueRefFast(oldRef);

    // atomic store
    field.SetTargetObject(ref, order);
}

BaseObject* MSMarkBarrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field,
                                               BaseObject* newRef, MemoryOrder order) const
{
    (void)obj;

    // atomic exchange
    MAddress oldValue = field.Exchange(newRef, order);
    RefField<false> oldField(oldValue);
    BaseObject* oldRef = oldField.GetTargetObject();

    // discover(old): the swapped-out reference
    EnqueueRefFast(oldRef);

    return oldRef;
}

bool MSMarkBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                            BaseObject* oldRef, BaseObject* newRef,
                                            MemoryOrder succOrder, MemoryOrder failOrder) const
{
    (void)obj;

    bool success = field.CompareExchange(oldRef, newRef, succOrder, failOrder);
    if (success) {
        // CAS succeeded: preserve the deleted edge only.
        EnqueueRefFast(oldRef);
    }
    return success;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
