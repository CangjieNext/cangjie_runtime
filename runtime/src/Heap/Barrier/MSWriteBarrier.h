// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_WRITE_BARRIER_H
#define MRT_MS_WRITE_BARRIER_H

#ifdef MRT_USE_MARK_SWEEP

#include "Barrier.h"
#include "WriteBarrierBuffer.h"

namespace MapleRuntime {

class Mutator;

// MSWriteBarrier — Hybrid write barrier for MS-GC concurrent marking phase.
//
// Semantics (Go-style hybrid barrier):
//   Write(slot, new):
//     shade(old = *slot)   // deletion barrier — prevents losing reachable whites
//     shade(new)           // insertion barrier — catches new grey roots
//     *slot = new
//
//   shade(ptr):
//     if ptr != null && IsHeapAddress(ptr) && !IsMarked(ptr):
//       MarkObject(ptr)    // atomic CAS on mark bitmap
//       wbBuf.Push(ptr)    // buffer for later draining by GC thread
//
// Reads: zero-overhead direct load (no fromSpace/toSpace in MS-GC).
//
// The write barrier is installed during GC_PHASE_MS_MARK_SETUP,
// GC_PHASE_MS_CONCURRENT_MARK, and GC_PHASE_MS_MARK_TERMINATION phases.
class MSWriteBarrier : public Barrier {
public:
    explicit MSWriteBarrier(Collector& collector) : Barrier(collector) {}
    ~MSWriteBarrier() override = default;

    // ── Reads: direct load, zero overhead ──
    BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const override;
    BaseObject* ReadStaticRef(RefField<false>& field) const override;
    BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const override;
    void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const override;
    void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const override;

    // ── Writes: shade(old) + shade(new) + store ──
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

    // ── Atomic operations: shade(old) + shade(new) ──
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
    // Core helper: mark object in bitmap, push to per-Mutator WBBuf if newly marked.
    // This is the "shade" operation in the hybrid barrier.
    static void MarkAndEnqueue(BaseObject* obj, WriteBarrierBuffer& wbBuf);

    // Get the current Mutator's WriteBarrierBuffer.
    static WriteBarrierBuffer& GetWBBuf();
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_WRITE_BARRIER_H
