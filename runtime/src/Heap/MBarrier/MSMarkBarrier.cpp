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
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace MapleRuntime {

// ---------------------------------------------------------------------------
// EnqueueRefFast stats — thread-local count to avoid cache-line bouncing on
// the hot write-barrier path; summed into a global atomic only at thread exit.
// ---------------------------------------------------------------------------
static thread_local uint64_t tl_enqueueRefFast_count = 0;
static std::atomic<uint64_t> g_enqueueRefFast_count{0};

namespace {
struct EnqueueRefFastStatsPrinter {
    ~EnqueueRefFastStatsPrinter()
    {
        // Flush this thread's local counter into the global sum.
        g_enqueueRefFast_count.fetch_add(tl_enqueueRefFast_count, std::memory_order_relaxed);
        tl_enqueueRefFast_count = 0;
        // Only the last destructor to run will see the full total; that is
        // acceptable for a diagnostic counter.
        uint64_t cnt = g_enqueueRefFast_count.load(std::memory_order_relaxed);
        fprintf(stderr,
                "[MS-GC][STATS] EnqueueRefFast: calls=%lu\n",
                static_cast<unsigned long>(cnt));
    }
};
static EnqueueRefFastStatsPrinter g_enqueueRefFast_printer;
} // namespace
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

static std::atomic<int32_t> g_invalidBarrierRefLogBudget{64};

static __attribute__((unused)) bool IsBarrierInvalidRefDiagEnabled()
{
    static bool enabled = []() {
        const char* env = std::getenv("CJ_MS_BARRIER_INVALID_REF_DIAG");
        return env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

static __attribute__((unused)) const char* GetBarrierDiagTypeName(const BaseObject* obj)
{
    if (obj == nullptr || obj->GetTypeInfo() == nullptr || obj->GetTypeInfo()->GetName() == nullptr) {
        return "<null>";
    }
    return obj->GetTypeInfo()->GetName();
}

static __attribute__((unused)) uintptr_t LoadBarrierDiagStateWord(const BaseObject* obj)
{
    return obj == nullptr ? 0 : *reinterpret_cast<const uintptr_t*>(obj);
}

static inline void EnqueueRefFast(BaseObject* ref)
{
    ++tl_enqueueRefFast_count;
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
    if (obj != nullptr && obj->GetTypeInfo() != nullptr) {
        obj->ForEachRefInStruct(
            [obj](RefField<false>& refField) {
                BaseObject* oldRef = refField.GetTargetObject();
                EnqueueRefFast(oldRef);
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
}

void MSMarkBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                      const GCTib gctib) const
{
    // discover(old): preserve all existing reference fields via GCTib bitmap
    gctib.ForEachBitmapWord(dst, [](RefField<>& refField) {
        BaseObject* oldRef = refField.GetTargetObject();
        EnqueueRefFast(oldRef);
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
        EnqueueRefFast(oldRef);
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
                EnqueueRefFast(oldRef);
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
        // CAS succeeded: discover the old edge (SATB deletion barrier).
        EnqueueRefFast(oldRef);
    }
    return success;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
