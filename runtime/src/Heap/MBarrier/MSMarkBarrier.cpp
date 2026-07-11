// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MSMarkBarrier.h"
#include "Heap/Barrier/Barrier.inline.h"
#include "Heap/Heap.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MCollector/MarkSweepCollector.h"
#include "Heap/MCollector/MSMarkQueuePolicy.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include <cstdlib>
#include <cstring>

namespace MapleRuntime {
static inline void AppendMarkedRefInto(MarkSweepCollector* collector,
                                       AllocBufferMS* allocBuf, BaseObject* ref)
{
    WorkBuf* log = collector->EnsureActiveLog(*allocBuf);
    if (UNLIKELY(log->Full())) {
        collector->PublishAndSwapLog(*allocBuf);
        log = allocBuf->GetActiveLog();
    }
    log->objs[log->len++] = ref;
}

// Route-B (Step37): append a raw old-ref into the owner's activeLog_ WorkBuf.
// No discover here — that happens on the consumer side. On overflow the full
// buffer is published wholesale (O(1)) and a fresh one installed.
// Pre-resolved (collector, allocBuf) form so bulk paths resolve the owner once
// per call instead of once per ref (R-1). collector must be non-null.
static inline void EnqueueRefInto(MarkSweepCollector* collector,
                                  AllocBufferMS* allocBuf, BaseObject* ref)
{
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
        return;
    }
    // Route-A: shade (mark) the old-ref at write time. Only the mark-CAS winner
    // logs it — already-grey / non-traceable refs are dropped here (perfect
    // dedup at the source). The logged buffer therefore holds only grey objects,
    // so the consumer scans them directly (no deferred discover).
    bool newlyMarked = collector->ShadeRefForBarrier(ref);
    if (ShouldEnqueueMSMarkedObject(newlyMarked, newlyMarked && ref->HasRefField())) {
        AppendMarkedRefInto(collector, allocBuf, ref);
    }
}

static inline void EnqueueRefFast(BaseObject* ref)
{
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
        return;
    }
    // Only log during concurrent mark; outside it there is no SATB snapshot to
    // preserve, so drop the ref entirely (no buffering/clearing needed).
    MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
    if (collector == nullptr) {
        return;
    }
    AllocBufferMS* allocBuf = ThreadLocal::GetAllocBufferMS();
    if (UNLIKELY(allocBuf == nullptr)) {
        allocBuf = AllocBufferMS::GetOrCreateAllocBufferMS();
    }
    EnqueueRefInto(collector, allocBuf, ref);
}

// Bulk paths (struct/array copy) log many old-refs at once. Resolve the active
// collector + owner AllocBufferMS ONCE here (R-1) and reuse across all refs,
// instead of paying a TLS lookup per ref inside EnqueueRefFast.
// Returns false (nothing to log) when not marking.
static inline bool ResolveBulkLogOwner(MarkSweepCollector*& collector, AllocBufferMS*& allocBuf)
{
    collector = MarkSweepCollector::GetActiveCollector();
    if (collector == nullptr) {
        return false;
    }
    allocBuf = ThreadLocal::GetAllocBufferMS();
    if (UNLIKELY(allocBuf == nullptr)) {
        allocBuf = AllocBufferMS::GetOrCreateAllocBufferMS();
    }
    return true;
}

class BulkRefMarker {
public:
    BulkRefMarker(MarkSweepCollector* collector, AllocBufferMS* allocBuf)
        : collector_(collector), allocBuf_(allocBuf)
    {
    }

    ~BulkRefMarker() { FlushAll(); }

    void Add(BaseObject* ref)
    {
        if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
            return;
        }

        MSRegionInfo* info = nullptr;
        size_t wordIdx = 0;
        uint64_t bitMask = 0;
        if (!collector_->ResolveSmallRefForBarrier(ref, info, wordIdx, bitMask)) {
            bool newlyMarked = collector_->ShadeRefForBarrier(ref);
            if (ShouldEnqueueMSMarkedObject(newlyMarked, newlyMarked && ref->HasRefField())) {
                AppendMarkedRefInto(collector_, allocBuf_, ref);
            }
            return;
        }

        Entry* entry = FindOrAllocate(info, wordIdx);
        unsigned bitIdx = static_cast<unsigned>(__builtin_ctzll(bitMask));
        if ((entry->mask & bitMask) == 0) {
            entry->refs[bitIdx] = ref;
            entry->mask |= bitMask;
        }
    }

    void FlushAll()
    {
        for (size_t i = 0; i < kEntryCount; ++i) {
            Flush(entries_[i]);
        }
    }

private:
    static constexpr size_t kEntryCount = 2;

    struct Entry {
        MSRegionInfo* info = nullptr;
        size_t wordIdx = 0;
        uint64_t mask = 0;
        BaseObject* refs[64] = {};
    };

    Entry* FindOrAllocate(MSRegionInfo* info, size_t wordIdx)
    {
        for (size_t i = 0; i < kEntryCount; ++i) {
            if (entries_[i].info == info && entries_[i].wordIdx == wordIdx) {
                return &entries_[i];
            }
        }
        for (size_t i = 0; i < kEntryCount; ++i) {
            if (entries_[i].info == nullptr) {
                entries_[i].info = info;
                entries_[i].wordIdx = wordIdx;
                return &entries_[i];
            }
        }
        Flush(entries_[nextEvict_]);
        Entry* entry = &entries_[nextEvict_];
        nextEvict_ = (nextEvict_ + 1) % kEntryCount;
        entry->info = info;
        entry->wordIdx = wordIdx;
        return entry;
    }

    void Flush(Entry& entry)
    {
        if (entry.info == nullptr) {
            return;
        }
        uint64_t newBits = entry.info->MarkBitmapWord(entry.wordIdx, entry.mask);
        while (newBits != 0) {
            unsigned bitIdx = static_cast<unsigned>(__builtin_ctzll(newBits));
            BaseObject* ref = entry.refs[bitIdx];
            if (ref->HasRefField()) {
                AppendMarkedRefInto(collector_, allocBuf_, ref);
            }
            newBits &= newBits - 1;
        }
        entry = Entry {};
    }

    MarkSweepCollector* collector_;
    AllocBufferMS* allocBuf_;
    Entry entries_[kEntryCount];
    size_t nextEvict_ = 0;
};

static inline void EnqueueStructRefsInRange(BaseObject* obj, MAddress start, size_t size)
{
    if (obj == nullptr || obj->GetTypeInfo() == nullptr || size == 0) {
        return;
    }
    MarkSweepCollector* collector = nullptr;
    AllocBufferMS* allocBuf = nullptr;
    if (!ResolveBulkLogOwner(collector, allocBuf)) {
        return;
    }
    BulkRefMarker marker(collector, allocBuf);
    obj->ForEachRefInStruct(
        [&marker](RefField<false>& refField) {
            marker.Add(refField.GetTargetObject());
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
    MarkSweepCollector* collector = nullptr;
    AllocBufferMS* allocBuf = nullptr;
    if (!ResolveBulkLogOwner(collector, allocBuf)) {
        return;
    }
    BulkRefMarker marker(collector, allocBuf);
    array->ForEachRefFieldInRange(
        [&marker](RefField<false>& refField) {
            marker.Add(refField.GetTargetObject());
        },
        start, start + size);
}

static inline void EnqueueStaticStructRefs(MAddress start, const GCTib gctib)
{
    MarkSweepCollector* collector = nullptr;
    AllocBufferMS* allocBuf = nullptr;
    if (!ResolveBulkLogOwner(collector, allocBuf)) {
        return;
    }
    BulkRefMarker marker(collector, allocBuf);
    gctib.ForEachBitmapWord(start, [&marker](RefField<>& refField) {
        marker.Add(refField.GetTargetObject());
    });
}

static inline void EnqueueRefArrayRange(MAddress start, size_t size)
{
    MarkSweepCollector* collector = nullptr;
    AllocBufferMS* allocBuf = nullptr;
    if (!ResolveBulkLogOwner(collector, allocBuf)) {
        return;
    }
    size_t refCount = size / sizeof(MAddress);
    RefField<false>* refs = reinterpret_cast<RefField<false>*>(start);
    BulkRefMarker marker(collector, allocBuf);
    for (size_t i = 0; i < refCount; ++i) {
        marker.Add(refs[i].GetTargetObject());
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
// Writes: deletion-style barrier — log(old) + store
// ============================================================================

void MSBarrierOps::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref)
{
    (void)obj;

    BaseObject* oldRef = field.GetTargetObject();
    if (oldRef == ref) {
        field.SetTargetObject(ref);
        return;
    }

    // log(old)
    EnqueueRefFast(oldRef);

    // store
    field.SetTargetObject(ref);
}

void MSBarrierOps::WriteStaticRef(RefField<false>& field, BaseObject* ref)
{
    BaseObject* oldRef = field.GetTargetObject();
    if (oldRef == ref) {
        field.SetTargetObject(ref);
        return;
    }

    // log(old)
    EnqueueRefFast(oldRef);

    // store
    field.SetTargetObject(ref);
}

void MSBarrierOps::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen)
{
    // log(old): preserve all existing reference fields in the destination struct
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

void MSBarrierOps::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                     const GCTib gctib)
{
    // log(old): preserve all existing reference fields via GCTib bitmap
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
// Array copy: discover overwritten refs before they are replaced
// ============================================================================

void MSBarrierOps::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                BaseObject* srcObj, MAddress srcField, MIndex srcSize)
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

void MSBarrierOps::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                   BaseObject* srcObj, MAddress srcField, MIndex srcSize)
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

void MSMarkBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    MSBarrierOps::WriteReference(obj, field, ref);
}

void MSMarkBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const
{
    MSBarrierOps::WriteStaticRef(field, ref);
}

void MSMarkBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    MSBarrierOps::WriteStruct(obj, dst, dstLen, src, srcLen);
}

void MSMarkBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                      const GCTib gctib) const
{
    MSBarrierOps::WriteStaticStruct(dst, dstLen, src, srcLen, gctib);
}

void MSMarkBarrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                 BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    MSBarrierOps::CopyRefArray(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
}

void MSMarkBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                    BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    MSBarrierOps::CopyStructArray(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
}

// ============================================================================
// Atomic operations: discover(old)
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
        // CAS succeeded: preserve the deleted edge.
        EnqueueRefFast(oldRef);
    }
    return success;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
