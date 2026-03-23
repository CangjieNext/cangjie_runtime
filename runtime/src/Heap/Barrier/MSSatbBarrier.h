// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_SATB_BARRIER_H
#define MRT_MS_SATB_BARRIER_H

#ifdef MRT_USE_MARK_SWEEP

#include "Barrier.h"

namespace MapleRuntime {

class Mutator;

// MSSatbBarrier — SATB deletion write barrier for MS-GC concurrent marking.
//
// Semantics (pure SATB deletion barrier):
//   Write(slot, new):
//     shade(old = *slot)   // preserve snapshot-at-the-beginning
//     *slot = new          // store
//
// shade(old) prevents the lost-object problem when a reference is overwritten
// during concurrent marking.  Combined with root scanning at mark-start and
// SATB buffer draining, this guarantees all objects reachable at the snapshot
// are eventually marked.
class MSSatbBarrier : public Barrier {
public:
    explicit MSSatbBarrier(Collector& collector) : Barrier(collector) {}
    ~MSSatbBarrier() override = default;

    // ── Reads: direct load, zero overhead ──
    BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const override;
    BaseObject* ReadStaticRef(RefField<false>& field) const override;
    BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const override;
    void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const override;
    void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const override;

    // ── Writes: shade(old) + store ──
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

    // ── Atomic operations: shade(old) + store ──
    BaseObject* AtomicReadReference(BaseObject* obj, RefField<true>& field,
                                    MemoryOrder order) const override;
    void AtomicWriteReference(BaseObject* obj, RefField<true>& field,
                              BaseObject* ref, MemoryOrder order) const override;
    BaseObject* AtomicSwapReference(BaseObject* obj, RefField<true>& field,
                                    BaseObject* newRef, MemoryOrder order) const override;
    bool CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                 BaseObject* oldRef, BaseObject* newRef,
                                 MemoryOrder succOrder, MemoryOrder failOrder) const override;

private:
    // Shade old value: push to SatbBuffer via Mutator::RememberObjectInSatbBuffer.
    static void ShadeOld(BaseObject* oldRef);
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_SATB_BARRIER_H
