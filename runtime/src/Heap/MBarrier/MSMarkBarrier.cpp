// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MSMarkBarrier.h"
#include "Heap/Barrier/Barrier.inline.h"
#include "Heap/Heap.h"
#include "Heap/MCollector/MarkSweepCollector.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include <atomic>
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

static bool IsHashMapCounterEnabled()
{
    static bool enabled = []() {
        const char* env = std::getenv("CJ_MS_HASHMAP_COUNTERS");
        return env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

static std::atomic<uint64_t> g_oldShadeCount{0};
static std::atomic<uint64_t> g_newEnqueueCount{0};
static std::atomic<uint64_t> g_newWriteRefCount{0};
static std::atomic<uint64_t> g_newWriteStaticRefCount{0};
static std::atomic<uint64_t> g_newWriteStructCount{0};
static std::atomic<uint64_t> g_newWriteStaticStructCount{0};
static std::atomic<uint64_t> g_newCopyRefArrayCount{0};
static std::atomic<uint64_t> g_newCopyStructArrayCount{0};
static std::atomic<uint64_t> g_newAtomicWriteCount{0};
static std::atomic<uint64_t> g_newAtomicSwapCount{0};
static std::atomic<uint64_t> g_newCasCount{0};
static std::atomic<int32_t> g_invalidBarrierRefLogBudget{64};

static const char* GetBarrierDiagTypeName(const BaseObject* obj)
{
    if (obj == nullptr || obj->GetTypeInfo() == nullptr || obj->GetTypeInfo()->GetName() == nullptr) {
        return "<null>";
    }
    return obj->GetTypeInfo()->GetName();
}

static uintptr_t LoadBarrierDiagStateWord(const BaseObject* obj)
{
    return obj == nullptr ? 0 : *reinterpret_cast<const uintptr_t*>(obj);
}

static void EnqueueRefWithDiag(const char* source, const BaseObject* holder, const void* slot, BaseObject* ref)
{
    if (ref != nullptr && Heap::IsHeapAddress(ref)) {
        bool traceable = true;
        uintptr_t stateWord = traceable ? LoadBarrierDiagStateWord(ref) : 0;
        if (UNLIKELY(!traceable || stateWord == 0 || !ref->IsValidObject())) {
            int32_t budget = g_invalidBarrierRefLogBudget.fetch_sub(1, std::memory_order_relaxed);
            if (budget > 0) {
                VLOG(REPORT,
                     "[MS-GC] Barrier invalid-ref source=%s holder=%p holderType=%s slot=%p ref=%p stateWord=0x%zx",
                     source,
                     static_cast<const void*>(holder),
                     GetBarrierDiagTypeName(holder),
                     slot,
                     static_cast<void*>(ref),
                     stateWord);
            }
        }
    }
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
        return;
    }
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) {
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

extern "C" void MSMarkBarrier_DumpAndResetCounters()
{
    if (!IsHashMapCounterEnabled()) {
        return;
    }
    uint64_t oldShade = g_oldShadeCount.exchange(0, std::memory_order_relaxed);
    uint64_t newEnqueue = g_newEnqueueCount.exchange(0, std::memory_order_relaxed);
    uint64_t writeRef = g_newWriteRefCount.exchange(0, std::memory_order_relaxed);
    uint64_t writeStaticRef = g_newWriteStaticRefCount.exchange(0, std::memory_order_relaxed);
    uint64_t writeStruct = g_newWriteStructCount.exchange(0, std::memory_order_relaxed);
    uint64_t writeStaticStruct = g_newWriteStaticStructCount.exchange(0, std::memory_order_relaxed);
    uint64_t copyRef = g_newCopyRefArrayCount.exchange(0, std::memory_order_relaxed);
    uint64_t copyStruct = g_newCopyStructArrayCount.exchange(0, std::memory_order_relaxed);
    uint64_t atomicWrite = g_newAtomicWriteCount.exchange(0, std::memory_order_relaxed);
    uint64_t atomicSwap = g_newAtomicSwapCount.exchange(0, std::memory_order_relaxed);
    uint64_t cas = g_newCasCount.exchange(0, std::memory_order_relaxed);

    VLOG(REPORT, "[MS-HM-COUNT] old=%llu new=%llu wr=%llu wsr=%llu wst=%llu wsst=%llu cra=%llu csa=%llu aw=%llu as=%llu cas=%llu",
         (unsigned long long)oldShade,
         (unsigned long long)newEnqueue,
         (unsigned long long)writeRef,
         (unsigned long long)writeStaticRef,
         (unsigned long long)writeStruct,
         (unsigned long long)writeStaticStruct,
         (unsigned long long)copyRef,
         (unsigned long long)copyStruct,
         (unsigned long long)atomicWrite,
         (unsigned long long)atomicSwap,
         (unsigned long long)cas);

}

// ============================================================================
// Core helpers
// ============================================================================

void MSMarkBarrier::EnqueueRef(BaseObject* ref)
{
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
        return;
    }
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr) {
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
// Writes: pure SATB — shade(old) + store
// ============================================================================

void MSMarkBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    (void)obj;

    // discover(old)
    BaseObject* oldRef = field.GetTargetObject();
    if (IsHashMapCounterEnabled()) {
        g_oldShadeCount.fetch_add(1, std::memory_order_relaxed);
    }
    EnqueueRefWithDiag("WriteReference.old", obj, &field, oldRef);

    // store
    field.SetTargetObject(ref);

    // discover(new): ensure the newly linked graph becomes visible this cycle
    if (IsHashMapCounterEnabled()) {
        g_newWriteRefCount.fetch_add(1, std::memory_order_relaxed);
        g_newEnqueueCount.fetch_add(1, std::memory_order_relaxed);
    }
    EnqueueRefWithDiag("WriteReference.new", obj, &field, ref);
}

void MSMarkBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const
{
    // discover(old)
    BaseObject* oldRef = field.GetTargetObject();
    if (IsHashMapCounterEnabled()) {
        g_oldShadeCount.fetch_add(1, std::memory_order_relaxed);
    }
    EnqueueRefWithDiag("WriteStaticRef.old", nullptr, &field, oldRef);

    // store
    field.SetTargetObject(ref);

    // discover(new)
    if (IsHashMapCounterEnabled()) {
        g_newWriteStaticRefCount.fetch_add(1, std::memory_order_relaxed);
        g_newEnqueueCount.fetch_add(1, std::memory_order_relaxed);
    }
    EnqueueRefWithDiag("WriteStaticRef.new", nullptr, &field, ref);
}

void MSMarkBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    // discover(old): preserve all existing reference fields in the destination struct
    // Guard: skip shade if TypeInfo is NULL (prematurely reclaimed / uninitialized object)
    if (obj != nullptr && obj->GetTypeInfo() != nullptr) {
        obj->ForEachRefInStruct(
            [obj](RefField<false>& refField) {
                BaseObject* oldRef = refField.GetTargetObject();
                EnqueueRefWithDiag("WriteStruct.old", obj, &refField, oldRef);
            },
            dst, dst + dstLen);
    }

    // memcpy (overwrites dst with src)
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                          reinterpret_cast<void*>(src), srcLen) == EOK,
                 "MSMarkBarrier WriteStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    CHECK_EQ(srcLen, dstLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif

    // discover(new): traverse the newly-written reference fields in the
    // destination struct after the copy.
    if (obj != nullptr && obj->GetTypeInfo() != nullptr) {
        obj->ForEachRefInStruct(
            [obj](RefField<false>& refField) {
                if (IsHashMapCounterEnabled()) {
                    g_newWriteStructCount.fetch_add(1, std::memory_order_relaxed);
                    g_newEnqueueCount.fetch_add(1, std::memory_order_relaxed);
                }
                BaseObject* newRef = refField.GetTargetObject();
                EnqueueRefWithDiag("WriteStruct.new", obj, &refField, newRef);
            },
            dst, dst + dstLen);
    }
}

void MSMarkBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                      const GCTib gctib) const
{
    // discover(old): preserve all existing reference fields via GCTib bitmap
    gctib.ForEachBitmapWord(dst, [](RefField<>& refField) {
        BaseObject* oldRef = refField.GetTargetObject();
        EnqueueRefWithDiag("WriteStaticStruct.old", nullptr, &refField, oldRef);
    });

    // memcpy
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                          reinterpret_cast<void*>(src), srcLen) == EOK,
                 "MSMarkBarrier WriteStaticStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstLen < srcLen ? dstLen : srcLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), copyLen);
#endif

    // discover(new): traverse the newly-written fields after the copy.
    gctib.ForEachBitmapWord(dst, [](RefField<>& refField) {
        if (IsHashMapCounterEnabled()) {
            g_newWriteStaticStructCount.fetch_add(1, std::memory_order_relaxed);
            g_newEnqueueCount.fetch_add(1, std::memory_order_relaxed);
        }
        BaseObject* newRef = refField.GetTargetObject();
        EnqueueRefWithDiag("WriteStaticStruct.new", nullptr, &refField, newRef);
    });
}

// ============================================================================
// Array copy: shade all overwritten refs (deletion barrier only)
// ============================================================================

void MSMarkBarrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                 BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    (void)dstObj;
    (void)srcObj;

    // discover(old): preserve all existing references in the destination range
    size_t refCount = dstSize / sizeof(MAddress);
    RefField<false>* dstRefs = reinterpret_cast<RefField<false>*>(dstField);
    for (size_t i = 0; i < refCount; ++i) {
        BaseObject* oldRef = dstRefs[i].GetTargetObject();
        EnqueueRefWithDiag("CopyRefArray.old", dstObj, &dstRefs[i], oldRef);
    }

    // memmove
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                           reinterpret_cast<void*>(srcField), srcSize) == EOK,
                 "MSMarkBarrier CopyRefArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif

    // discover(new): preserve the newly-copied references for this cycle's closure.
    for (size_t i = 0; i < refCount; ++i) {
        if (IsHashMapCounterEnabled()) {
            g_newCopyRefArrayCount.fetch_add(1, std::memory_order_relaxed);
            g_newEnqueueCount.fetch_add(1, std::memory_order_relaxed);
        }
        BaseObject* newRef = dstRefs[i].GetTargetObject();
        EnqueueRefWithDiag("CopyRefArray.new", dstObj, &dstRefs[i], newRef);
    }
}

void MSMarkBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                    BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    (void)srcObj;

    // discover(old): preserve all existing reference fields in the destination range
    if (dstObj != nullptr) {
        dstObj->ForEachRefInStruct(
            [dstObj](RefField<false>& refField) {
                BaseObject* oldRef = refField.GetTargetObject();
                EnqueueRefWithDiag("CopyStructArray.old", dstObj, &refField, oldRef);
            },
            dstField, dstField + dstSize);
    }

    // memmove
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                           reinterpret_cast<void*>(srcField), srcSize) == EOK,
                 "MSMarkBarrier CopyStructArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif

    // discover(new): preserve all newly-copied reference fields in the destination range.
    if (dstObj != nullptr) {
        dstObj->ForEachRefInStruct(
            [dstObj](RefField<false>& refField) {
                if (IsHashMapCounterEnabled()) {
                    g_newCopyStructArrayCount.fetch_add(1, std::memory_order_relaxed);
                    g_newEnqueueCount.fetch_add(1, std::memory_order_relaxed);
                }
                BaseObject* newRef = refField.GetTargetObject();
                EnqueueRefWithDiag("CopyStructArray.new", dstObj, &refField, newRef);
            },
            dstField, dstField + dstSize);
    }
}

// ============================================================================
// Atomic operations: shade(old) only
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
    if (IsHashMapCounterEnabled()) {
        g_oldShadeCount.fetch_add(1, std::memory_order_relaxed);
    }
    EnqueueRefWithDiag("AtomicWriteReference.old", obj, &field, oldRef);

    // atomic store
    field.SetTargetObject(ref, order);

    // discover(new)
    if (IsHashMapCounterEnabled()) {
        g_newAtomicWriteCount.fetch_add(1, std::memory_order_relaxed);
        g_newEnqueueCount.fetch_add(1, std::memory_order_relaxed);
    }
    EnqueueRefWithDiag("AtomicWriteReference.new", obj, &field, ref);
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
    if (IsHashMapCounterEnabled()) {
        g_oldShadeCount.fetch_add(1, std::memory_order_relaxed);
    }
    EnqueueRefWithDiag("AtomicSwapReference.old", obj, &field, oldRef);
    // discover(new)
    if (IsHashMapCounterEnabled()) {
        g_newAtomicSwapCount.fetch_add(1, std::memory_order_relaxed);
        g_newEnqueueCount.fetch_add(1, std::memory_order_relaxed);
    }
    EnqueueRefWithDiag("AtomicSwapReference.new", obj, &field, newRef);

    return oldRef;
}

bool MSMarkBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                            BaseObject* oldRef, BaseObject* newRef,
                                            MemoryOrder succOrder, MemoryOrder failOrder) const
{
    (void)obj;

    bool success = field.CompareExchange(oldRef, newRef, succOrder, failOrder);
    if (success) {
        // CAS succeeded: discover the old edge and the new edge.
        if (IsHashMapCounterEnabled()) {
            g_oldShadeCount.fetch_add(1, std::memory_order_relaxed);
        }
        EnqueueRefWithDiag("CompareAndSwapReference.old", obj, &field, oldRef);
        if (IsHashMapCounterEnabled()) {
            g_newCasCount.fetch_add(1, std::memory_order_relaxed);
            g_newEnqueueCount.fetch_add(1, std::memory_order_relaxed);
        }
        EnqueueRefWithDiag("CompareAndSwapReference.new", obj, &field, newRef);
    }
    return success;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
