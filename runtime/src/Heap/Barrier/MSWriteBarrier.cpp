// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MSWriteBarrier.h"
#include "Barrier.inline.h"
#include "Heap/Allocator/LargeObjectSpace.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/MarkSweepCollector.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "Base/Log.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {

// Diagnostic counters for write barrier
static std::atomic<uint64_t> wbCallCount{0};
static std::atomic<uint64_t> wbNullOrNonHeap{0};
static std::atomic<uint64_t> wbPhaseSkip{0};
static std::atomic<uint64_t> wbAlreadyMarked{0};
static std::atomic<uint64_t> wbEnqueued{0};

extern "C" void MSWriteBarrier_DumpStats()
{
    LOG(RTLOG_ERROR, "[MS-WB-DIAG] MarkAndEnqueue calls: %llu, nullOrNonHeap: %llu, "
        "phaseSkip: %llu, alreadyMarked: %llu, enqueued: %llu",
        (unsigned long long)wbCallCount.load(),
        (unsigned long long)wbNullOrNonHeap.load(),
        (unsigned long long)wbPhaseSkip.load(),
        (unsigned long long)wbAlreadyMarked.load(),
        (unsigned long long)wbEnqueued.load());
}

extern "C" void MSWriteBarrier_ResetStats()
{
    wbCallCount.store(0);
    wbNullOrNonHeap.store(0);
    wbPhaseSkip.store(0);
    wbAlreadyMarked.store(0);
    wbEnqueued.store(0);
}

// ============================================================================
// Core: MarkAndEnqueue (the "shade" operation)
// ============================================================================

void MSWriteBarrier::MarkAndEnqueue(BaseObject* obj, WriteBarrierBuffer& wbBuf)
{
    if (obj == nullptr) {
        return;
    }
    // Check both contiguous heap AND LOS (mmap'd huge objects).
    if (!Heap::IsHeapAddress(obj)) {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
        LargeObjectSpace* los = space.GetLargeObjectSpace();
        if (los == nullptr || !los->Contains(reinterpret_cast<uintptr_t>(obj))) {
            return;
        }
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
        // Newly marked → push to per-Mutator write barrier buffer.
        // If buffer is full, flush the ENTIRE buffer to the collector's global
        // work queue (one mutex acquisition for all items), then push the new
        // item into the now-empty buffer.  This prevents:
        //   a) silent overflow (grey objects never traced → children swept)
        //   b) per-item global pushes (expensive repeated mutex locks)
        if (UNLIKELY(wbBuf.IsFull())) {
            MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
            if (collector != nullptr) {
                collector->FlushWriteBarrierOverflow(wbBuf);
            } else {
                wbBuf.Clear();
            }
        }
        wbBuf.Push(obj);
    }
}

// EnqueueForRescan: unconditionally push obj into WB buffer for re-scanning.
// Unlike MarkAndEnqueue, this BYPASSES the mark-bit check.  Used for the
// shade(new) container-rescan path: when a container object is already marked
// (mark-at-alloc), MarkAndEnqueue would be a no-op, but we still need its
// children traced at MarkTermination.
void MSWriteBarrier::EnqueueForRescan(BaseObject* obj, WriteBarrierBuffer& wbBuf)
{
    if (obj == nullptr) {
        return;
    }
    // Phase gate — only enqueue during active marking.
    GCPhase phase = Mutator::GetMutator()->GetMutatorPhase();
    if (phase != GCPhase::GC_PHASE_MS_MARK_SETUP &&
        phase != GCPhase::GC_PHASE_MS_CONCURRENT_MARK &&
        phase != GCPhase::GC_PHASE_MS_MARK_TERMINATION) {
        return;
    }
    // Handle overflow before push.
    if (UNLIKELY(wbBuf.IsFull())) {
        MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
        if (collector != nullptr) {
            collector->FlushWriteBarrierOverflow(wbBuf);
        } else {
            wbBuf.Clear();
        }
    }
    wbBuf.Push(obj);
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
// Writes: hybrid barrier — shade(old) + store + shade(new)
// shade(old): SATB deletion barrier — preserves snapshot-at-the-beginning
// shade(new): Dijkstra insertion barrier — ensures newly-assigned references
// are traced, preventing mark-at-alloc objects from hiding white descendants
// ============================================================================

void MSWriteBarrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    // shade(old): SATB deletion barrier — preserve snapshot
    BaseObject* oldRef = field.GetTargetObject();
    MarkAndEnqueue(oldRef, wbBuf);

    // store
    field.SetTargetObject(ref);

    // shade(new): unconditionally enqueue the CONTAINER object for re-scan.
    // Why bypass mark check: mark-at-alloc objects are already marked, so
    // MarkAndEnqueue(obj) would be a no-op.  But their newly-assigned children
    // may never have been traced.  By pushing obj to the WB buffer, MarkTermination
    // will re-scan obj's fields and discover the entire new sub-graph.
    if (obj != nullptr) {
        EnqueueForRescan(obj, wbBuf);
    } else {
        // Static root or no container — fall back to shading the ref directly
        MarkAndEnqueue(ref, wbBuf);
    }
}

void MSWriteBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const
{
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    // shade(old): SATB deletion barrier
    BaseObject* oldRef = field.GetTargetObject();
    MarkAndEnqueue(oldRef, wbBuf);

    // store
    field.SetTargetObject(ref);

    // shade(new): Dijkstra insertion barrier
    MarkAndEnqueue(ref, wbBuf);
}

void MSWriteBarrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    // shade(old): mark all existing reference fields in the destination struct
    if (obj != nullptr) {
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

    // shade(new): mark newly-written reference fields (Dijkstra insertion barrier)
    if (obj != nullptr) {
        obj->ForEachRefInStruct(
            [&wbBuf](RefField<false>& refField) {
                BaseObject* newRef = refField.GetTargetObject();
                MarkAndEnqueue(newRef, wbBuf);
            },
            dst, dst + dstLen);
    }
}

void MSWriteBarrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen,
                                       const GCTib gctib) const
{
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    // shade(old): mark all existing reference fields via GCTib bitmap
    gctib.ForEachBitmapWord(dst, [&wbBuf](RefField<>& refField) {
        BaseObject* oldRef = refField.GetTargetObject();
        MarkAndEnqueue(oldRef, wbBuf);
    });

    // memcpy
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen,
                          reinterpret_cast<void*>(src), srcLen) == EOK,
                 "MSWriteBarrier WriteStaticStruct memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstLen < srcLen ? dstLen : srcLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), copyLen);
#endif

    // shade(new): mark newly-written reference fields (Dijkstra insertion barrier)
    gctib.ForEachBitmapWord(dst, [&wbBuf](RefField<>& refField) {
        BaseObject* newRef = refField.GetTargetObject();
        MarkAndEnqueue(newRef, wbBuf);
    });
}

// ============================================================================
// Array copy: shade all overwritten refs + shade all new refs
// ============================================================================

void MSWriteBarrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                  BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    (void)dstObj;
    (void)srcObj;

    WriteBarrierBuffer& wbBuf = GetWBBuf();
    size_t refCount = dstSize / sizeof(MAddress);
    RefField<false>* dstRefs = reinterpret_cast<RefField<false>*>(dstField);

    // shade(old): mark all existing references in the destination range
    for (size_t i = 0; i < refCount; ++i) {
        BaseObject* oldRef = dstRefs[i].GetTargetObject();
        MarkAndEnqueue(oldRef, wbBuf);
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

    // shade(new): mark newly-written references (Dijkstra insertion barrier)
    for (size_t i = 0; i < refCount; ++i) {
        BaseObject* newRef = dstRefs[i].GetTargetObject();
        MarkAndEnqueue(newRef, wbBuf);
    }
}

void MSWriteBarrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                     BaseObject* srcObj, MAddress srcField, MIndex srcSize) const
{
    (void)srcObj;

    WriteBarrierBuffer& wbBuf = GetWBBuf();

    // shade(old): mark all existing reference fields in the destination range
    // before overwriting them (SATB deletion barrier requirement).
    // Without this, old references in struct array elements are silently
    // overwritten, breaking the snapshot-at-the-beginning invariant.
    if (dstObj != nullptr) {
        dstObj->ForEachRefInStruct(
            [&wbBuf](RefField<false>& refField) {
                BaseObject* oldRef = refField.GetTargetObject();
                MarkAndEnqueue(oldRef, wbBuf);
            },
            dstField, dstField + dstSize);
    }

    // memmove
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize,
                           reinterpret_cast<void*>(srcField), srcSize) == EOK,
                 "MSWriteBarrier CopyStructArray memmove_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif

    // shade(new): mark newly-written reference fields (Dijkstra insertion barrier)
    if (dstObj != nullptr) {
        dstObj->ForEachRefInStruct(
            [&wbBuf](RefField<false>& refField) {
                BaseObject* newRef = refField.GetTargetObject();
                MarkAndEnqueue(newRef, wbBuf);
            },
            dstField, dstField + dstSize);
    }
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
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    // shade(old): SATB deletion barrier
    RefField<false> oldField(field.GetFieldValue(std::memory_order_relaxed));
    BaseObject* oldRef = oldField.GetTargetObject();
    MarkAndEnqueue(oldRef, wbBuf);

    // atomic store
    field.SetTargetObject(ref, order);

    // shade(new): enqueue container to trigger re-scan
    if (obj != nullptr) {
        EnqueueForRescan(obj, wbBuf);
    } else {
        MarkAndEnqueue(ref, wbBuf);
    }
}

BaseObject* MSWriteBarrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field,
                                                BaseObject* newRef, MemoryOrder order) const
{
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    // atomic exchange
    MAddress oldValue = field.Exchange(newRef, order);
    RefField<false> oldField(oldValue);
    BaseObject* oldRef = oldField.GetTargetObject();

    // shade(old) — the swapped-out reference (SATB deletion barrier)
    MarkAndEnqueue(oldRef, wbBuf);

    // shade(new): enqueue container to trigger re-scan
    if (obj != nullptr) {
        EnqueueForRescan(obj, wbBuf);
    } else {
        MarkAndEnqueue(newRef, wbBuf);
    }

    return oldRef;
}

bool MSWriteBarrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field,
                                             BaseObject* oldRef, BaseObject* newRef,
                                             MemoryOrder succOrder, MemoryOrder failOrder) const
{
    WriteBarrierBuffer& wbBuf = GetWBBuf();

    bool success = field.CompareExchange(oldRef, newRef, succOrder, failOrder);
    if (success) {
        // CAS succeeded: shade old ref (SATB deletion barrier)
        MarkAndEnqueue(oldRef, wbBuf);
        // shade(new): enqueue container to trigger re-scan
        if (obj != nullptr) {
            EnqueueForRescan(obj, wbBuf);
        } else {
            MarkAndEnqueue(newRef, wbBuf);
        }
    }
    // If CAS failed, no write happened — no barrier needed
    return success;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
