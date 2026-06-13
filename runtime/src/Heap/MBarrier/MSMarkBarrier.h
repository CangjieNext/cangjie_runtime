// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_MARK_BARRIER_H
#define MRT_MS_MARK_BARRIER_H

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/Barrier/Barrier.h"

namespace MapleRuntime {

class Mutator;

// MSMarkBarrier — unified old/new write barrier for MSGC concurrent marking.
//
// Semantics:
//   Write(slot, new):
//     enqueue(old = *slot) // discover overwritten edge target
//     *slot = new          // store
//     enqueue(new)         // discover newly linked edge target
//
// Both old/new refs try to discover the target object and enqueue it into the
// per-mutator concurrent mark queue. Newly allocated objects are marked after
// their header is initialized and scanned once during mark termination.
class MSMarkBarrier : public Barrier {
public:
    explicit MSMarkBarrier(Collector& collector) : Barrier(collector) {}
    ~MSMarkBarrier() override = default;

    // ── Reads: direct load, zero overhead ──
    BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const override;
    BaseObject* ReadStaticRef(RefField<false>& field) const override;
    BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const override;
    void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const override;
    void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const override;

    // ── Writes: enqueue(old) + store + enqueue(new) ──
    void WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const override;
    void WriteStaticRef(RefField<false>& field, BaseObject* ref) const override;
    void WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const override;
    void WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                           const GCTib gctib) const override;

    // ── Array copy: shade all overwritten refs ──
    void CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                      BaseObject* srcObj, MAddress srcField, MIndex srcSize) const override;
    void CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                         BaseObject* srcObj, MAddress srcField, MIndex srcSize) const override;

    // ── Atomic operations: enqueue(old) + store + enqueue(new) ──
    BaseObject* AtomicReadReference(BaseObject* obj, RefField<true>& field,
                                    MemoryOrder order) const override;
    void AtomicWriteReference(BaseObject* obj, RefField<true>& field,
                              BaseObject* ref, MemoryOrder order) const override;
    BaseObject* AtomicSwapReference(BaseObject* obj, RefField<true>& field,
                                    BaseObject* newRef, MemoryOrder order) const override;
    bool CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                 BaseObject* oldRef, BaseObject* newRef,
                                 MemoryOrder succOrder, MemoryOrder failOrder) const override;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_MARK_BARRIER_H
