// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MSWriteBarrier.h"
#include "Barrier.inline.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {

// ============================================================================
// Core: MarkAndEnqueue (the "shade" operation)
// ============================================================================

void MSWriteBarrier::MarkAndEnqueue(BaseObject* obj, WriteBarrierBuffer& wbBuf)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return;
    }
    // Only shade during active marking phases (SATB barrier).
    // Between GC cycles (MS_SWEEP phase), the barrier is a no-op to avoid
    // accumulating spurious marks that make sweep think everything is live.
    GCPhase phase = Mutator::GetMutator()->GetMutatorPhase();
    if (phase != GCPhase::GC_PHASE_MS_MARK_SETUP &&
        phase != GCPhase::GC_PHASE_MS_CONCURRENT_MARK &&
        phase != GCPhase::GC_PHASE_MS_MARK_TERMINATION) {
        return;
    }
    // RegionSpace::MarkObject returns true if ALREADY marked, false if NEWLY marked.
    if (!RegionSpace::MarkObject(obj)) {
        // Newly marked → push to per-Mutator write barrier buffer
        wbBuf.Push(obj);
    }
}

WriteBarrierBuffer& MSWriteBarrier::GetWBBuf()
{
    return Mutator::GetMutator()->GetWriteBarrierBuffer();
}

// ============================================================================
// Reads: zero-overhead direct load
// ============================================================================

BaseObject* MSWriteBarrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    (void)obj;
    return field.GetTargetObject();
}

BaseObject* MSWriteBarrier::ReadStaticRef(RefField<false>& field) const
{
    return field.GetTargetObject();
}

BaseObject* MSWriteBarrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    (void)obj;
    return field.GetTargetObject();
}

void MSWriteBarrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{
    (void)obj;
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size,
                          reinterpret_cast<void*>(src), size) == EOK,
                 "MSWriteBarrier ReadStruct memcpy_s failed");
}

void MSWriteBarrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    (void)gctib;
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size,
                          reinterpret_cast<void*>(src), size) == EOK,
                 "MSWriteBarrier ReadStaticStruct memcpy_s failed");
}

// ============================================================================
// Writes: pure SATB deletion barrier — shade(old) + store
// New references are NOT shaded here; newly allocated objects are marked
// at allocation time (mark-at-alloc) during GC marking phases.
// ============================================================================

void MSWriteBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    (void)obj;
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    // shade(old): SATB deletion barrier — preserve snapshot
    BaseObject* oldRef = field.GetTargetObject();
    MarkAndEnqueue(oldRef, wbBuf);

    // store
    field.SetTargetObject(ref);
}

void MSWriteBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const
{
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    // shade(old): SATB deletion barrier
    BaseObject* oldRef = field.GetTargetObject();
    MarkAndEnqueue(oldRef, wbBuf);

    // store
    field.SetTargetObject(ref);
}

void MSWriteBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    // shade(old): mark all existing reference fields in the destination struct
    if (obj != nullptr) {
        WriteBarrierBuffer& wbBuf = GetWBBuf();
        obj->ForEachRefInStruct(
            [&wbBuf](RefField<false>& refField) {
                BaseObject* oldRef = refField.GetTargetObject();
                MarkAndEnqueue(oldRef, wbBuf);
            },
            dst, dst + dstLen);
    }

    // memcpy (overwrites dst with src)
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                          reinterpret_cast<void*>(src), srcLen) == EOK,
                 "MSWriteBarrier WriteStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    CHECK_EQ(srcLen, dstLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif

    // No shade(new) for struct writes: SATB deletion barrier only.
    // New references will be traced by the GC thread's marking pass.
}

void MSWriteBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                       const GCTib gctib) const
{
    // shade(old): mark all existing reference fields via GCTib bitmap
    {
        WriteBarrierBuffer& wbBuf = GetWBBuf();
        gctib.ForEachBitmapWord(dst, [&wbBuf](RefField<>& refField) {
            BaseObject* oldRef = refField.GetTargetObject();
            MarkAndEnqueue(oldRef, wbBuf);
        });
    }

    // memcpy
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                          reinterpret_cast<void*>(src), srcLen) == EOK,
                 "MSWriteBarrier WriteStaticStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstLen < srcLen ? dstLen : srcLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), copyLen);
#endif

    // No shade(new) for static struct writes: SATB deletion barrier only.
}

// ============================================================================
// Array copy: shade all overwritten refs + shade all new refs
// ============================================================================

void MSWriteBarrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                  BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    (void)dstObj;
    (void)srcObj;

    // shade(old): mark all existing references in the destination range
    {
        WriteBarrierBuffer& wbBuf = GetWBBuf();
        size_t refCount = dstSize / sizeof(MAddress);
        RefField<false>* dstRefs = reinterpret_cast<RefField<false>*>(dstField);
        for (size_t i = 0; i < refCount; ++i) {
            BaseObject* oldRef = dstRefs[i].GetTargetObject();
            MarkAndEnqueue(oldRef, wbBuf);
        }
    }

    // memmove
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                           reinterpret_cast<void*>(srcField), srcSize) == EOK,
                 "MSWriteBarrier CopyRefArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif

    // No shade(new) for array copy: SATB deletion barrier only.
}

void MSWriteBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                     BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    // Struct arrays may contain reference fields but we don't have per-element GCTib here.
    // Conservatively do a plain copy — struct array refs will be discovered by the GC
    // thread's trace phase. This matches the base Barrier behavior.
    (void)dstObj;
    (void)srcObj;
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                           reinterpret_cast<void*>(srcField), srcSize) == EOK,
                 "MSWriteBarrier CopyStructArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
}

// ============================================================================
// Atomic operations
// ============================================================================

BaseObject* MSWriteBarrier::AtomicReadReference(BaseObject* obj, RefField<true>& field,
                                                MemoryOrder order) const
{
    // Reads are zero-overhead in MS-GC
    (void)obj;
    RefField<false> tmpField(field.GetFieldValue(order));
    return tmpField.GetTargetObject();
}

void MSWriteBarrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field,
                                          BaseObject* ref, MemoryOrder order) const
{
    (void)obj;
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    // shade(old): SATB deletion barrier
    RefField<false> oldField(field.GetFieldValue(std::memory_order_relaxed));
    BaseObject* oldRef = oldField.GetTargetObject();
    MarkAndEnqueue(oldRef, wbBuf);

    // atomic store
    field.SetTargetObject(ref, order);
}

BaseObject* MSWriteBarrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field,
                                                BaseObject* newRef, MemoryOrder order) const
{
    (void)obj;
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    // atomic exchange
    MAddress oldValue = field.Exchange(newRef, order);
    RefField<false> oldField(oldValue);
    BaseObject* oldRef = oldField.GetTargetObject();

    // shade(old) — the swapped-out reference (SATB deletion barrier)
    MarkAndEnqueue(oldRef, wbBuf);

    return oldRef;
}

bool MSWriteBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                             BaseObject* oldRef, BaseObject* newRef,
                                             MemoryOrder succOrder, MemoryOrder failOrder) const
{
    (void)obj;
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    bool success = field.CompareExchange(oldRef, newRef, succOrder, failOrder);
    if (success) {
        // CAS succeeded: shade old ref (SATB deletion barrier)
        MarkAndEnqueue(oldRef, wbBuf);
    }
    // If CAS failed, no write happened — no barrier needed
    return success;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
