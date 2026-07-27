// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MSMarkBarrier.h"
#include "Heap/Barrier/Barrier.inline.h"
#include "Heap/Heap.h"
#include "Heap/MBarrier/MSRefField.inline.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MCollector/MarkSweepCollector.h"
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
    log->Objects()[log->len++] = ref;
}

// Append an already shaded old-ref into the owner's activeLog_ WorkBuf.
// On overflow the full buffer is published wholesale (O(1)) and a fresh one
// installed. The caller must pass the active collector and current owner.
static inline void EnqueueRefInto(MarkSweepCollector* collector,
                                  AllocBufferMS* allocBuf, BaseObject* ref)
{
    // Route-A: shade (mark) the old-ref at write time. Only the mark-CAS winner
    // logs it — already-grey / non-traceable refs are dropped here (perfect
    // dedup at the source). The logged buffer therefore holds only grey objects,
    // so the consumer scans them directly (no deferred discover).
    TypeInfo* typeInfo = nullptr;
    bool newlyMarked = collector->ShadeManagedRefForBarrier(ref, &typeInfo);
    if (newlyMarked && typeInfo->HasRefField()) {
        AppendMarkedRefInto(collector, allocBuf, ref);
    }
}

static inline bool ResolveLogOwner(MarkSweepCollector*& collector, AllocBufferMS*& allocBuf)
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

static inline void EnqueueRefFast(BaseObject* ref)
{
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
        return;
    }
    // Only log during concurrent mark; outside it there is no SATB snapshot to
    // preserve, so drop the ref entirely (no buffering/clearing needed).
    MarkSweepCollector* collector = nullptr;
    AllocBufferMS* allocBuf = nullptr;
    if (!ResolveLogOwner(collector, allocBuf)) {
        return;
    }
    EnqueueRefInto(collector, allocBuf, ref);
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
        TypeInfo* typeInfo = nullptr;
        if (!collector_->ResolveManagedSmallRefForBarrier(
                ref, info, wordIdx, bitMask, typeInfo)) {
            bool newlyMarked = collector_->ShadeManagedRefForBarrier(ref, &typeInfo);
            if (newlyMarked && typeInfo->HasRefField()) {
                AppendMarkedRefInto(collector_, allocBuf_, ref);
            }
            return;
        }

        Entry* entry = FindOrAllocate(info, wordIdx);
        unsigned bitIdx = static_cast<unsigned>(__builtin_ctzll(bitMask));
        if ((entry->mask & bitMask) == 0) {
            entry->refs[bitIdx] = ref;
            entry->typeInfos[bitIdx] = typeInfo;
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
        TypeInfo* typeInfos[64] = {};
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
            TypeInfo* typeInfo = entry.typeInfos[bitIdx];
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
            MRT_ASSERT(typeInfo != nullptr,
                       "bulk mark winner must retain its validated TypeInfo");
#endif
            if (typeInfo->HasRefField()) {
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
    if (!ResolveLogOwner(collector, allocBuf)) {
        return;
    }
    BulkRefMarker marker(collector, allocBuf);
    obj->ForEachRefInStruct(
        [&marker](RefField<false>& refField) {
            marker.Add(MSLoadRawReference(refField));
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
    if (!ResolveLogOwner(collector, allocBuf)) {
        return;
    }
    BulkRefMarker marker(collector, allocBuf);
    array->ForEachRefFieldInRange(
        [&marker](RefField<false>& refField) {
            marker.Add(MSLoadRawReference(refField));
        },
        start, start + size);
}

static inline void EnqueueStaticStructRefs(MAddress start, const GCTib gctib)
{
    MarkSweepCollector* collector = nullptr;
    AllocBufferMS* allocBuf = nullptr;
    if (!ResolveLogOwner(collector, allocBuf)) {
        return;
    }
    BulkRefMarker marker(collector, allocBuf);
    gctib.ForEachBitmapWord(start, [&marker](RefField<>& refField) {
        marker.Add(MSLoadRawReference(refField));
    });
}

static inline void EnqueueRefArrayRange(MAddress start, size_t size)
{
    MarkSweepCollector* collector = nullptr;
    AllocBufferMS* allocBuf = nullptr;
    if (!ResolveLogOwner(collector, allocBuf)) {
        return;
    }
    size_t refCount = size / sizeof(MAddress);
    RefField<false>* refs = reinterpret_cast<RefField<false>*>(start);
    BulkRefMarker marker(collector, allocBuf);
    for (size_t i = 0; i < refCount; ++i) {
        marker.Add(MSLoadRawReference(refs[i]));
    }
}

// ============================================================================
// Writes: deletion-style barrier — log(old) + store
// ============================================================================

static inline void WriteRefField(RefField<false>& field, BaseObject* ref)
{
    BaseObject* oldRef = MSLoadRawReference(field);
    if (oldRef != ref) {
        EnqueueRefFast(oldRef);
    }
    MSStoreRawReference(field, ref);
}

void MSBarrierOps::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref)
{
    (void)obj;
    WriteRefField(field, ref);
}

void MSBarrierOps::WriteStaticRef(RefField<false>& field, BaseObject* ref)
{
    WriteRefField(field, ref);
}

void MSBarrierOps::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen)
{
    // log(old): preserve all existing reference fields in the destination struct
    size_t copiedLen = (dstLen < srcLen ? dstLen : srcLen);
    EnqueueStructRefsInRange(obj, dst, copiedLen);

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
    size_t copiedLen = (dstSize < srcSize ? dstSize : srcSize);
    EnqueueRefArrayRange(dstField, copiedLen);

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

void MSMarkBarrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field,
                                         BaseObject* ref, MemoryOrder order) const
{
    (void)obj;

    // discover(old)
    BaseObject* oldRef = MSLoadRawReference(field, std::memory_order_relaxed);
    EnqueueRefFast(oldRef);

    // atomic store
    MSStoreRawReference(field, ref, order);

}

BaseObject* MSMarkBarrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field,
                                               BaseObject* newRef, MemoryOrder order) const
{
    (void)obj;

    // atomic exchange
    BaseObject* oldRef = MSExchangeRawReference(field, newRef, order);

    // discover(old): the swapped-out reference
    EnqueueRefFast(oldRef);

    return oldRef;
}

bool MSMarkBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                            BaseObject* oldRef, BaseObject* newRef,
                                            MemoryOrder succOrder, MemoryOrder failOrder) const
{
    (void)obj;

    bool success = MSCompareExchangeRawReference(field, oldRef, newRef, succOrder, failOrder);
    if (success) {
        // CAS succeeded: preserve the deleted edge.
        EnqueueRefFast(oldRef);
    }
    return success;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
