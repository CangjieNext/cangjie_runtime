// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_IDLE_BARRIER_H
#define MRT_MS_IDLE_BARRIER_H

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/Barrier/Barrier.h"
#include "securec.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {

// MSIdleBarrier — Zero-overhead barrier for GC idle / sweep phases.
//
// All reads:  direct load (no self-healing needed — MS has no fromSpace/toSpace)
// All writes: direct store (no shading needed — no concurrent marking active)
//
// This replaces the base Barrier (which does CopyGC-style TryUpdateRefField
// on reads) with clean direct memory access.
class MSIdleBarrier : public Barrier {
public:
    explicit MSIdleBarrier(Collector& collector) : Barrier(collector) {}
    ~MSIdleBarrier() override = default;

    // ── Reads: direct load, no barrier ──

    BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const override
    {
        (void)obj;
        return field.GetTargetObject();
    }

    BaseObject* ReadStaticRef(RefField<false>& field) const override
    {
        return field.GetTargetObject();
    }

    BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const override
    {
        (void)obj;
        return field.GetTargetObject();
    }

    void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const override
    {
        (void)obj;
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size,
                              reinterpret_cast<void*>(src), size) == EOK,
                     "MSIdleBarrier ReadStruct memcpy_s failed");
    }

    void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const override
    {
        (void)gctib;
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size,
                              reinterpret_cast<void*>(src), size) == EOK,
                     "MSIdleBarrier ReadStaticStruct memcpy_s failed");
    }

    // ── Writes: direct store, no barrier ──

    void WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const override
    {
        (void)obj;
        field.SetTargetObject(ref);
    }

    void WriteStaticRef(RefField<false>& field, BaseObject* ref) const override
    {
        field.SetTargetObject(ref);
    }

    void WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const override
    {
        (void)obj;
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                              reinterpret_cast<void*>(src), srcLen) == EOK,
                     "MSIdleBarrier WriteStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        CHECK_EQ(srcLen, dstLen);
        Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
        Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
    }

    void WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                           const GCTib gctib) const override
    {
        (void)gctib;
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                              reinterpret_cast<void*>(src), srcLen) == EOK,
                     "MSIdleBarrier WriteStaticStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        size_t copyLen = (dstLen < srcLen ? dstLen : srcLen);
        Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), copyLen);
        Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), copyLen);
#endif
    }

    void CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                      BaseObject* srcObj, MAddress srcField, MIndex srcSize) const override
    {
        (void)dstObj;
        (void)srcObj;
        CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                               reinterpret_cast<void*>(srcField), srcSize) == EOK,
                     "MSIdleBarrier CopyRefArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
        Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
        Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
    }

    void CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                         BaseObject* srcObj, MAddress srcField, MIndex srcSize) const override
    {
        (void)dstObj;
        (void)srcObj;
        CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                               reinterpret_cast<void*>(srcField), srcSize) == EOK,
                     "MSIdleBarrier CopyStructArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
        Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
        Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
    }

    // ── Atomic operations: direct, no barrier ──

    BaseObject* AtomicReadReference(BaseObject* obj, RefField<true>& field,
                                    MemoryOrder order) const override
    {
        (void)obj;
        RefField<false> tmpField(field.GetFieldValue(order));
        return tmpField.GetTargetObject();
    }

    void AtomicWriteReference(BaseObject* obj, RefField<true>& field,
                              BaseObject* ref, MemoryOrder order) const override
    {
        (void)obj;
        field.SetTargetObject(ref, order);
    }

    BaseObject* AtomicSwapReference(BaseObject* obj, RefField<true>& field,
                                    BaseObject* newRef, MemoryOrder order) const override
    {
        (void)obj;
        MAddress oldValue = field.Exchange(newRef, order);
        RefField<false> oldField(oldValue);
        return oldField.GetTargetObject();
    }

    bool CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                 BaseObject* oldRef, BaseObject* newRef,
                                 MemoryOrder succOrder, MemoryOrder failOrder) const override
    {
        (void)obj;
        return field.CompareExchange(oldRef, newRef, succOrder, failOrder);
    }
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_IDLE_BARRIER_H
