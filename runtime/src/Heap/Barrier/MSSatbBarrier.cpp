// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MSSatbBarrier.h"
#include "Barrier.inline.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {

// ============================================================================
// Core: Shade — push reference to SatbBuffer (SATB deletion barrier)
// ============================================================================

void MSSatbBarrier::ShadeOld(BaseObject* oldRef)
{
    if (oldRef == nullptr) {
        return;
    }
    Mutator* mutator = Mutator::GetMutator();
    mutator->RememberObjectInSatbBuffer(oldRef);
}

// ============================================================================
// Reads: zero-overhead direct load
// ============================================================================

BaseObject* MSSatbBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    (void)obj;
    return field.GetTargetObject();
}

BaseObject* MSSatbBarrier::ReadStaticRef(RefField<false>& field) const
{
    return field.GetTargetObject();
}

BaseObject* MSSatbBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    (void)obj;
    return field.GetTargetObject();
}

void MSSatbBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{
    (void)obj;
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size,
                          reinterpret_cast<void*>(src), size) == EOK,
                 "MSSatbBarrier ReadStruct memcpy_s failed");
}

void MSSatbBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    (void)gctib;
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size,
                          reinterpret_cast<void*>(src), size) == EOK,
                 "MSSatbBarrier ReadStaticStruct memcpy_s failed");
}

// ============================================================================
// Writes: pure SATB — shade(old) + store
// ============================================================================

void MSSatbBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    (void)obj;

    // shade(old): SATB deletion barrier — preserve snapshot
    BaseObject* oldRef = field.GetTargetObject();
    ShadeOld(oldRef);

    // store
    field.SetTargetObject(ref);
}

void MSSatbBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const
{
    // shade(old): SATB deletion barrier
    BaseObject* oldRef = field.GetTargetObject();
    ShadeOld(oldRef);

    // store
    field.SetTargetObject(ref);
}

void MSSatbBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    // shade(old): mark all existing reference fields in the destination struct
    // Guard: skip shade if TypeInfo is NULL (prematurely reclaimed / uninitialized object)
    if (obj != nullptr && obj->GetTypeInfo() != nullptr) {
        obj->ForEachRefInStruct(
            [](RefField<false>& refField) {
                BaseObject* oldRef = refField.GetTargetObject();
                ShadeOld(oldRef);
            },
            dst, dst + dstLen);
    }

    // memcpy (overwrites dst with src)
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                          reinterpret_cast<void*>(src), srcLen) == EOK,
                 "MSSatbBarrier WriteStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    CHECK_EQ(srcLen, dstLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

void MSSatbBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                      const GCTib gctib) const
{
    // shade(old): mark all existing reference fields via GCTib bitmap
    gctib.ForEachBitmapWord(dst, [](RefField<>& refField) {
        BaseObject* oldRef = refField.GetTargetObject();
        ShadeOld(oldRef);
    });

    // memcpy
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                          reinterpret_cast<void*>(src), srcLen) == EOK,
                 "MSSatbBarrier WriteStaticStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstLen < srcLen ? dstLen : srcLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), copyLen);
#endif
}

// ============================================================================
// Array copy: shade all overwritten refs (deletion barrier only)
// ============================================================================

void MSSatbBarrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                 BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    (void)dstObj;
    (void)srcObj;

    // shade(old): preserve all existing references in the destination range
    size_t refCount = dstSize / sizeof(MAddress);
    RefField<false>* dstRefs = reinterpret_cast<RefField<false>*>(dstField);
    for (size_t i = 0; i < refCount; ++i) {
        BaseObject* oldRef = dstRefs[i].GetTargetObject();
        ShadeOld(oldRef);
    }

    // memmove
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                           reinterpret_cast<void*>(srcField), srcSize) == EOK,
                 "MSSatbBarrier CopyRefArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
}

void MSSatbBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                    BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    (void)srcObj;

    // shade(old): preserve all existing reference fields in the destination range
    if (dstObj != nullptr) {
        dstObj->ForEachRefInStruct(
            [](RefField<false>& refField) {
                BaseObject* oldRef = refField.GetTargetObject();
                ShadeOld(oldRef);
            },
            dstField, dstField + dstSize);
    }

    // memmove
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                           reinterpret_cast<void*>(srcField), srcSize) == EOK,
                 "MSSatbBarrier CopyStructArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
}

// ============================================================================
// Atomic operations: shade(old) only
// ============================================================================

BaseObject* MSSatbBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field,
                                               MemoryOrder order) const
{
    (void)obj;
    RefField<false> tmpField(field.GetFieldValue(order));
    return tmpField.GetTargetObject();
}

void MSSatbBarrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field,
                                         BaseObject* ref, MemoryOrder order) const
{
    (void)obj;

    // shade(old): SATB deletion barrier
    RefField<false> oldField(field.GetFieldValue(std::memory_order_relaxed));
    BaseObject* oldRef = oldField.GetTargetObject();
    ShadeOld(oldRef);

    // atomic store
    field.SetTargetObject(ref, order);
}

BaseObject* MSSatbBarrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field,
                                               BaseObject* newRef, MemoryOrder order) const
{
    (void)obj;

    // atomic exchange
    MAddress oldValue = field.Exchange(newRef, order);
    RefField<false> oldField(oldValue);
    BaseObject* oldRef = oldField.GetTargetObject();

    // shade(old): the swapped-out reference
    ShadeOld(oldRef);

    return oldRef;
}

bool MSSatbBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                            BaseObject* oldRef, BaseObject* newRef,
                                            MemoryOrder succOrder, MemoryOrder failOrder) const
{
    (void)obj;

    bool success = field.CompareExchange(oldRef, newRef, succOrder, failOrder);
    if (success) {
        // CAS succeeded: shade old ref (SATB deletion barrier)
        ShadeOld(oldRef);
    }
    return success;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
