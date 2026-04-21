// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/MAllocator/MSRegionSpace.h"

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSRegionManager.h"
#include "Heap/MAllocator/MSSizeClass.h"
#include "Collector/Collector.h"
#include "Collector/CollectorResources.h"
#include "Common/ScopedObjectAccess.h"
#include "ExceptionManager.h"
#include "Heap.h"
#include "Heap/MCollector/MarkSweepCollector.h"
#include "Heap/MBarrier/WriteBarrierBuffer.h"
#include "Mutator/Mutator.h"
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {

static inline bool IsMSMarkingPhase(GCPhase phase)
{
    return phase == GCPhase::GC_PHASE_MS_MARK_SETUP ||
           phase == GCPhase::GC_PHASE_MS_CONCURRENT_MARK ||
           phase == GCPhase::GC_PHASE_MS_MARK_TERMINATION;
}

// ============================================================
// RegionSpaceMS destructor
// ============================================================
RegionSpaceMS::~RegionSpaceMS()
{
    delete msCentralPool_;
    msCentralPool_ = nullptr;
    delete msRegionManager_;
    msRegionManager_ = nullptr;
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
    Sanitizer::OnHeapDeallocated(map->GetBaseAddr(), map->GetMappedSize());
#endif
    MemMap::DestroyMemMap(map);
}

// ============================================================
// Shared allocation logic (copied from RegionSpace)
// ============================================================
bool RegionSpaceMS::ShouldRetryAllocation(size_t& tryTimes, size_t size) const
{
    if (!IsRuntimeThread() && tryTimes <= static_cast<size_t>(TryAllocationThreshold::RESCHEDULE)) {
        CJThreadResched();
        return true;
    }
    if (tryTimes < static_cast<size_t>(TryAllocationThreshold::TRIGGER_OOM)) {
        if (Heap::GetHeap().IsGcStarted()) {
            ScopedEnterSaferegion enterSaferegion(false);
            Heap::GetHeap().GetCollectorResources().WaitForGCFinish();
        } else {
            Heap::GetHeap().GetCollector().RequestGC(GC_REASON_HEU, false);
        }
        return true;
    }
    if (tryTimes == static_cast<size_t>(TryAllocationThreshold::TRIGGER_OOM)) {
        if (!Heap::GetHeap().IsGcStarted()) {
            VLOG(REPORT, "gc is triggered for OOM");
            Heap::GetHeap().GetCollector().RequestGC(GC_REASON_OOM, false);
        } else {
            ScopedEnterSaferegion enterSaferegion(false);
            Heap::GetHeap().GetCollectorResources().WaitForGCFinish();
            tryTimes--;
        }
        return true;
    }
    VLOG(REPORT, "Cannot allocate memory of %zu(B), throw an OutOfMemory exception", size);
    ExceptionManager::OutOfMemory();
    return false;
}

MAddress RegionSpaceMS::Allocate(size_t size, AllocType allocType)
{
    (void)allocType;
    size_t allocSize = ToAllocSize(size);
    uintptr_t internalAddr = 0;

    if (UNLIKELY(allocSize >= kLargeObjectThreshold)) {
        internalAddr = TryAllocateOnceMS(allocSize);
        if (UNLIKELY(internalAddr == 0)) {
            if (IsGcThread()) {
                return 0;
            }
            VLOG(REPORT, "Large allocation cannot allocate memory of %zu(B), throw an OutOfMemory exception", size);
            ExceptionManager::OutOfMemory();
            return 0;
        }
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanAllocObject(reinterpret_cast<void *>(internalAddr), allocSize);
#endif
        return internalAddr + HEADER_SIZE;
    }

    size_t tryTimes = 0;
    do {
        tryTimes++;
        internalAddr = TryAllocateOnceMS(allocSize);
        if (LIKELY(internalAddr != 0)) {
            break;
        }
        if (IsGcThread()) {
            return 0;
        }
        if (!ShouldRetryAllocation(tryTimes, size)) {
            break;
        }
        (void)sched_yield();
    } while (true);
    if (internalAddr == 0) {
        return 0;
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanAllocObject(reinterpret_cast<void *>(internalAddr), allocSize);
#endif
    return internalAddr + HEADER_SIZE;
}

// ============================================================
// RegionSpaceMS virtual overrides
// ============================================================
MAddress RegionSpaceMS::GetSpaceEndAddress() const
{
    if (msRegionManager_ != nullptr) {
        return msRegionManager_->GetHeapEnd();
    }
    return reservedEnd;
}

size_t RegionSpaceMS::GetCurrentCapacity() const
{
    if (msRegionManager_ != nullptr) {
        return msRegionManager_->GetCurrentCapacityBytes();
    }
    return 0;
}

size_t RegionSpaceMS::GetMaxCapacity() const
{
    if (msRegionManager_ != nullptr) {
        return msRegionManager_->GetMaxHeapEnd() - reservedStart;
    }
    return reservedEnd - reservedStart;
}

size_t RegionSpaceMS::GetRecentAllocatedSize() const
{
    return 0;
}

size_t RegionSpaceMS::GetSurvivedSize() const
{
    return 0;
}

size_t RegionSpaceMS::GetUsedPageSize() const
{
    if (msCentralPool_ != nullptr) {
        return msCentralPool_->GetTotalMSRegionBytes();
    }
    return 0;
}

size_t RegionSpaceMS::LargeObjectBytes() const
{
    return 0;
}

size_t RegionSpaceMS::FromSpaceSize() const
{
    return 0;
}

size_t RegionSpaceMS::PinnedSpaceSize() const
{
    return 0;
}

size_t RegionSpaceMS::ReclaimGarbageMemory(bool releaseAll)
{
    if (msRegionManager_ == nullptr) {
        return 0;
    }
    size_t released = 0;
    if (msCentralPool_ != nullptr) {
        released += msCentralPool_->HandleBackgroundScavenge(releaseAll);
    }
    msRegionManager_->ShrinkHeap();
    return released;
}

bool RegionSpaceMS::HasPendingReclaimWork() const
{
    if (msCentralPool_ == nullptr) {
        return false;
    }
    size_t emptyCount = msCentralPool_->GetEmptySweptRegionCount();
    size_t highWatermark = msCentralPool_->GetEmptyCacheHighWatermarkRegions();
    if (emptyCount >= highWatermark) {
        return true;
    }
    size_t committedBytes = msCentralPool_->GetTotalMSRegionBytes();
    size_t targetBytes = msCentralPool_->GetTrueLiveSlackTargetBytes();
    return targetBytes != 0 && committedBytes > targetBytes && emptyCount > 0;
}

bool RegionSpaceMS::BackgroundSweep()
{
    return msCentralPool_ != nullptr && msCentralPool_->BackgroundSweep();
}

void RegionSpaceMS::BackgroundFillAllocCaches()
{
    if (msCentralPool_ != nullptr) {
        msCentralPool_->BackgroundFillAllocCaches();
    }
}

bool RegionSpaceMS::ForEachObj(const std::function<void(BaseObject*)>& visitor, bool safe) const
{
    (void)visitor;
    (void)safe;
    return true;
}

MAddress RegionSpaceMS::TryAllocateOnceMS(size_t allocSize)
{
    if (UNLIKELY(allocSize >= kLargeObjectThreshold)) {
        size_t unitCount = (allocSize + MRT_PAGE_SIZE - 1) / MRT_PAGE_SIZE;
        size_t regionBytes = ((unitCount * MRT_PAGE_SIZE) + MS_REGION_SIZE - 1)
                             / MS_REGION_SIZE * MS_REGION_SIZE;

        uintptr_t regionStart = 0;
        MSRegionInfo* baseInfo = msRegionManager_->TakeRegionForLarge(unitCount, regionStart, msCentralPool_);
        if (baseInfo == nullptr) {
            LOG(RTLOG_ERROR, "[MS-GC] TryAllocateOnceMS LARGE ALLOC FAILED: "
                "allocSize=%zu, unitCount=%zu, totalMSRegionBytes=%zu, "
                "msRegionManager freeUnits=%u",
                allocSize, unitCount,
                msCentralPool_ ? msCentralPool_->GetTotalMSRegionBytes() : 0,
                msRegionManager_->GetFreeUnitCount());
            return 0;
        }
        MAddress addr = static_cast<MAddress>(regionStart);
        RegisterLargeObjectMS(addr, regionBytes);
        // Pacing debt: large objects bypass AllocBufferMS::Allocate, so we must
        // accumulate assist debt explicitly here to keep GC pacing correct.
        AllocBufferMS* msBuffer = AllocBufferMS::GetAllocBufferMS();
        if (msBuffer != nullptr) {
            msBuffer->AccumulatePacingDebt(allocSize);
        }
        return addr;
    }

    AllocBufferMS* msBuffer = AllocBufferMS::GetOrCreateAllocBufferMS();
    if (msBuffer != nullptr) {
        MAddress addr = msBuffer->Allocate(allocSize);
        if (LIKELY(addr != 0)) {
            return addr;
        }
    }
    return 0;
}

void RegionSpaceMS::EnsureThreadLocalAllocBuffer()
{
    (void)AllocBufferMS::GetOrCreateAllocBufferMS();
}

size_t RegionSpaceMS::AllocatedBytes() const
{
    if (msCentralPool_ != nullptr) {
        return msCentralPool_->GetTotalMSRegionBytes();
    }
    return 0;
}

// ============================================================
// Static methods on Allocator base class (MS implementations)
// ============================================================
bool Allocator::IsMarkedObject(const BaseObject* obj)
{
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (MSRegionInfoArray::IsHeapAddress(addr)) {
        MSRegionInfo* msInfo = MSRegionInfoArray::GetRegionInfoValidated(addr);
        if (msInfo != nullptr) {
            if (msInfo->regionType == MSRegionType::MS_REGION_LARGE) {
                MSRegionInfo* base = MSRegionInfoArray::GetRegionInfoByIdx(
                    static_cast<size_t>(msInfo->reserved));
                return base->IsMarked();
            }
            if (msInfo->markBitmap != nullptr && msInfo->totalSlotCount > 0) {
                return msInfo->GetMarkBit(addr);
            }
        }
    }
    return false;
}

void RegionSpaceMS::Init(const HeapParam& vmHeapParam)
{
    MemMap::Option opt = MemMap::DEFAULT_OPTIONS;
    opt.tag = "cangjie_heap";
    size_t heapSize = vmHeapParam.heapSize * 1024;

    size_t maxVirtualHeapSize = MSRegionManager::ComputeMaxVirtualHeapSize(heapSize);
    size_t totalSize = MSRegionManager::GetVirtualReserveSize(maxVirtualHeapSize);
    opt.protAll = true;
#if defined(CANGJIE_ASAN_SUPPORT)
    opt.flags &= ~MAP_PRIVATE;
    opt.flags |= MAP_SHARED;
    DLOG(SANITIZER, "mmap flags set to 0x%x", opt.flags);
#endif
    map = MemMap::MapMemory(totalSize, totalSize, opt);
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
    Sanitizer::OnHeapAllocated(map->GetBaseAddr(), map->GetMappedSize());
#endif

    Logger::GetLogger().SetMinimumLogLevel(CangjieRuntime::GetLogParam().logLevel);
    MAddress metadata = reinterpret_cast<MAddress>(map->GetBaseAddr());

    msRegionManager_ = new (std::nothrow) MSRegionManager();
    CHECK_DETAIL(msRegionManager_ != nullptr, "failed to create MSRegionManager");
    msRegionManager_->Initialize(heapSize, metadata, maxVirtualHeapSize);

    reservedStart = msRegionManager_->GetHeapStart();
    reservedEnd = msRegionManager_->GetHeapEnd();

#if defined(MRT_DUMP_ADDRESS)
    VLOG(REPORT, "region metadata@%zx, heap @[0x%zx+%zu, 0x%zx)", metadata, reservedStart, reservedEnd - reservedStart,
         reservedEnd);
#endif
    Heap::OnHeapCreated(reservedStart);
    Heap::OnHeapExtended(reservedEnd);

    SizeClassInfo::Init();
    size_t maxVirtualRegionCount = msRegionManager_->GetMaxVirtualRegionCount();
    MSRegionInfoArray::Init(reservedStart, maxVirtualRegionCount, msRegionManager_->GetArrayBase());

    msCentralPool_ = new (std::nothrow) CentralRegionPool(msRegionManager_);
    CHECK_DETAIL(msCentralPool_ != nullptr, "failed to create CentralRegionPool");

    VLOG(REPORT, "[MS-GC] MS subsystem initialized: committedRegions=%zu, maxVirtualRegions=%zu, "
         "heapRange=[0x%zx, 0x%zx), maxHeapEnd=0x%zx",
         msRegionManager_->GetMaxRegionCount(), maxVirtualRegionCount,
         reservedStart, reservedEnd, msRegionManager_->GetMaxHeapEnd());
}

bool Allocator::MarkObject(const BaseObject* obj)
{
    MAddress addr = reinterpret_cast<MAddress>(obj);

    if (MSRegionInfoArray::IsHeapAddress(addr)) {
        return MSRegionInfoArray::MarkObjectInHeap(static_cast<uintptr_t>(addr));
    }

    return true;
}

bool Allocator::ShouldEnqueue(const BaseObject* obj)
{
    (void)obj;
    return true;
}

bool Allocator::IsResurrectedObject([[maybe_unused]] const BaseObject* obj)
{
    return false;
}

void Allocator::EnsureThreadLocalBuffer()
{
    AllocBufferMS* msBuffer = AllocBufferMS::GetOrCreateAllocBufferMS();
    // Set ThreadLocalData::buffer so LLVM fast-path (in pre-built SDK libs)
    // reads tlRegion_ (NullRegion) at offset 0 and falls to slow MCC_* call.
    if (ThreadLocal::GetAllocBuffer() == nullptr) {
        ThreadLocal::SetAllocBuffer(reinterpret_cast<AllocBuffer*>(msBuffer));
    }
}

void Allocator::CommitRawPointerRegions()
{
    AllocBufferMS* msBuffer = AllocBufferMS::GetAllocBufferMS();
    if (msBuffer != nullptr) {
        msBuffer->CommitRawPointerRegions();
    }
}

void Allocator::PushRootToBuffer(BaseObject* root)
{
    // Root buffering is only a staging step for MarkSetup handshake roots.
    // It must not be treated as grey-work production; the collector later
    // performs the white->grey transition explicitly when merging root buffers.
    AllocBufferMS* buffer = AllocBufferMS::GetOrCreateAllocBufferMS();
    if (buffer != nullptr) {
        buffer->PushRoot(root);
    }
}

void RegionSpaceMS::RegisterLargeObjectMS(MAddress addr, size_t regionBytes)
{
    uintptr_t start = static_cast<uintptr_t>(addr);
    uintptr_t end = start + regionBytes;

    MSRegionInfo* baseInfo = MSRegionInfoArray::GetRegionInfo(start);
    size_t baseIdx = MSRegionInfoArray::GetRegionIdx(baseInfo);

    size_t spanCount = (regionBytes + MS_REGION_SIZE - 1) / MS_REGION_SIZE;
    for (size_t i = 0; i < spanCount; i++) {
        MSRegionInfo* entry = MSRegionInfoArray::GetRegionInfoByIdx(baseIdx + i);
        if (entry->regionType != MSRegionType::MS_REGION_FREE &&
            entry->regionType != MSRegionType::MS_REGION_LARGE) {
            LOG(RTLOG_FATAL, "[MS-GC] RegisterLargeObjectMS: span[%zu/%zu] (idx=%zu) has "
                "non-free regionType=%d! allocPtr=0x%zx, regionEnd=0x%zx, "
                "allocBitmap=%p, sizeClass=%u, freeSlots=%u, totalSlots=%u, "
                "sweepState=%d, sweepGen=%u, nextIdx=%d, prevIdx=%d, reserved=0x%llx. "
                "Large: start=0x%zx, end=0x%zx, regionBytes=%zu",
                i, spanCount, baseIdx + i,
                (int)entry->regionType, entry->allocPtr, entry->regionEnd,
                (void*)entry->allocBitmap, (unsigned)entry->sizeClass,
                (unsigned)entry->freeSlotCount, (unsigned)entry->totalSlotCount,
                (int)entry->sweepState, entry->sweepGen,
                entry->nextRegionIdx, entry->prevRegionIdx,
                (unsigned long long)entry->reserved,
                start, end, regionBytes);
        }
    }

    baseInfo->InitForLargeObject(start, end);
    baseInfo->reserved = static_cast<uint64_t>(baseIdx);

    for (size_t i = 1; i < spanCount; i++) {
        MSRegionInfo* spanInfo = MSRegionInfoArray::GetRegionInfoByIdx(baseIdx + i);
        spanInfo->regionType = MSRegionType::MS_REGION_LARGE;
        spanInfo->markBitmap = nullptr;
        spanInfo->allocBitmap = nullptr;
        spanInfo->totalSlotCount = 0;
        spanInfo->freeSlotCount = 0;
        spanInfo->flags_ = 0;
        spanInfo->reserved = static_cast<uint64_t>(baseIdx);
    }

    baseInfo->AtomicSetMarked();

    // BLACK allocation for large objects during marking: the hybrid write
    // barrier (SATB + insertion) ensures children are independently
    // discovered when fields are written. No need to enqueue.

    if (msCentralPool_ != nullptr) {
        msCentralPool_->RegisterLargeRegion(baseInfo);
        msCentralPool_->AddLargeRegionBytes(regionBytes);
    }
}



#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
bool RegionSpaceMS::IsHeapObject(MAddress addr) const
{
    return IsHeapAddress(addr);
}
#endif

void RegionSpaceMS::FeedHungryBuffers()
{
    return;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
