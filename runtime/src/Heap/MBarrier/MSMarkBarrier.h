// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_MARK_BARRIER_H
#define MRT_MS_MARK_BARRIER_H

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MBarrier/MSIdleBarrier.h"

namespace MapleRuntime {

class Mutator;

enum class MSBarrierRuntimeStat : uint8_t {
    OLD_REF_RESOLVE_REJECTED,
    OLD_REF_THRESHOLD_FLUSH,
    OLD_REF_SET_CONFLICT_FLUSH,
    COUNT
};

void RecordMSBarrierRuntimeStat(MSBarrierRuntimeStat stat);
bool IsMSBarrierRuntimeStatsEnabled();

namespace MSBarrierOps {
void WriteReference(RefField<false>& field, BaseObject* ref);
void WriteStaticRef(RefField<false>& field, BaseObject* ref);
void WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen);
void WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib);
void CopyRefArray(MAddress dstField, MIndex dstSize,
                  MAddress srcField, MIndex srcSize);
void CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                     MAddress srcField, MIndex srcSize);
void AtomicWriteReference(RefField<true>& field, BaseObject* ref,
                          MemoryOrder order);
BaseObject* AtomicSwapReference(RefField<true>& field, BaseObject* newRef,
                                MemoryOrder order);
bool CompareAndSwapReference(RefField<true>& field, BaseObject* oldRef,
                             BaseObject* newRef,
                             MemoryOrder succOrder, MemoryOrder failOrder);
} // namespace MSBarrierOps

// MSMarkBarrier — deletion-style write barrier for MSGC concurrent marking.
//
// Semantics:
//   Write(slot, new):
//     enqueue(old = *slot) // discover overwritten edge target
//     *slot = new          // store
//
// Read operations are identical to MSIdleBarrier and are inherited directly.
class MSMarkBarrier : public MSIdleBarrier {
public:
    explicit MSMarkBarrier(Collector& collector) : MSIdleBarrier(collector) {}
    ~MSMarkBarrier() override = default;

    // ── Writes: enqueue(old) + store ──
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

    // ── Atomic writes: enqueue(old) + update ──
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
