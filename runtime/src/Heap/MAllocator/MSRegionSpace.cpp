// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/MAllocator/MSRegionSpace.h"

#ifdef MRT_USE_MARK_SWEEP

#include "Base/TimeUtils.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSOOMEmergencyPolicy.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSRegionManager.h"
#include "Heap/MAllocator/MSSizeClass.h"
#include "Collector/Collector.h"
#include "Collector/CollectorResources.h"
#include "Common/ScopedObjectAccess.h"
#include "ExceptionManager.h"
#include "Heap.h"
#include "Heap/MCollector/MarkSweepCollector.h"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include <limits>
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {

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

MAddress RegionSpaceMS::Allocate(size_t size, AllocType allocType)
{
    return Allocate(size, allocType, ThreadLocal::GetThreadLocalData());
}

MAddress RegionSpaceMS::Allocate(size_t size, AllocType allocType, ThreadLocalData* local)
{
    (void)allocType;
    size_t allocSize = ToAllocSize(size);
    if (UNLIKELY(SizeClassInfo::IsLargeObjectSize(allocSize))) {
        return AllocateLargeObject(size, allocSize, local);
    }

    return AllocateRegularObject(size, allocSize, local);
}

MAddress RegionSpaceMS::AllocateLargeObject(size_t size, size_t allocSize, ThreadLocalData* local)
{
    AllocBufferMS* msBuffer = local != nullptr ? local->msAllocBuffer : AllocBufferMS::GetAllocBufferMS();
    bool allowEmergency = !IsGcThread();
    uintptr_t internalAddr = TryMSAllocationWithEmergencyRetry(
        allowEmergency,
        [this, size, allocSize, local, msBuffer]() {
            return TryAllocateLargeObjectOnce(size, allocSize, local, msBuffer);
        },
        [size]() {
            VLOG(REPORT, "Large allocation failed for %zu(B), trigger one synchronous OOM GC", size);
            Heap::GetHeap().GetCollector().RequestGC(GC_REASON_OOM, false);
        });
    if (UNLIKELY(internalAddr == 0)) {
        if (!allowEmergency) {
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

MAddress RegionSpaceMS::AllocateRegularObject(size_t size, size_t allocSize, ThreadLocalData* local)
{
    AllocBufferMS* msBuffer = local != nullptr ? local->msAllocBuffer : AllocBufferMS::GetAllocBufferMS();
    bool allowEmergency = !IsGcThread();
    uintptr_t internalAddr = TryMSAllocationWithEmergencyRetry(
        allowEmergency,
        [this, allocSize, local, &msBuffer]() {
            return TryAllocateRegularObjectOnce(allocSize, local, msBuffer);
        },
        [size]() {
            VLOG(REPORT, "Regular allocation failed for %zu(B), trigger one synchronous OOM GC", size);
            Heap::GetHeap().GetCollector().RequestGC(GC_REASON_OOM, false);
        });
    if (UNLIKELY(internalAddr == 0)) {
        if (!allowEmergency) {
            return 0;
        }
        VLOG(REPORT, "Regular allocation cannot allocate memory of %zu(B), throw an OutOfMemory exception", size);
        ExceptionManager::OutOfMemory();
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

size_t RegionSpaceMS::GetUsedPageSize() const
{
    if (msCentralPool_ != nullptr) {
        return msCentralPool_->GetActiveMSRegionCount() * MS_REGION_SIZE;
    }
    return 0;
}

size_t RegionSpaceMS::LargeObjectBytes() const
{
    if (msCentralPool_ == nullptr) {
        return 0;
    }
    return msCentralPool_->GetLargeObjectBytes();
}

// ---------------------------------------------------------------------------
// Scavenge-related queries
// ---------------------------------------------------------------------------

size_t RegionSpaceMS::GetHeapInUseBytes() const
{
    return GetUsedPageSize();
}

size_t RegionSpaceMS::GetSweptHeapBytes() const
{
    if (msCentralPool_ == nullptr) {
        return 0;
    }
    return msCentralPool_->GetSweptMSRegionCount() * MS_REGION_SIZE;
}

size_t RegionSpaceMS::GetSweptHeapBytesReference() const
{
    if (msCentralPool_ == nullptr) {
        return 0;
    }
    return msCentralPool_->GetSweptMSRegionCountReference() * MS_REGION_SIZE;
}

size_t RegionSpaceMS::GetCentralCacheBytes() const
{
    if (msCentralPool_ == nullptr) {
        return 0;
    }
    size_t emptySwept = msCentralPool_->GetEmptySweptRegionCount() * MS_REGION_SIZE;
    size_t largeCache = msCentralPool_->GetLargeHotCacheBytes();
    return emptySwept + largeCache;
}

size_t RegionSpaceMS::GetHeapRetainedBytes() const
{
    if (msCentralPool_ == nullptr || msRegionManager_ == nullptr) {
        return 0;
    }
    size_t active = msCentralPool_->GetActiveMSRegionCount() * MS_REGION_SIZE;
    size_t empty = msCentralPool_->GetEmptySweptRegionCount() * MS_REGION_SIZE;
    size_t largeCache = msCentralPool_->GetLargeHotCacheBytes();
    size_t hotFree = msRegionManager_->GetHotFreeBytes();
    return active + empty + largeCache + hotFree;
}

size_t RegionSpaceMS::GetHeapRetainedBytesReference() const
{
    if (msCentralPool_ == nullptr || msRegionManager_ == nullptr) {
        return 0;
    }
    size_t active = msCentralPool_->GetActiveMSRegionCountReference() * MS_REGION_SIZE;
    size_t empty = msCentralPool_->GetEmptySweptRegionCount() * MS_REGION_SIZE;
    size_t largeCache = msCentralPool_->GetLargeHotCacheBytes();
    size_t hotFree = msRegionManager_->GetHotFreeBytesReference();
    return active + empty + largeCache + hotFree;
}

size_t RegionSpaceMS::GetHeapThresholdBytes() const
{
    return Heap::GetHeap().GetCollector().GetGCStats().GetThreshold();
}

size_t RegionSpaceMS::GetScavengeThresholdBytes() const
{
    return Heap::GetHeap().GetCollector().GetGCStats().GetScavengeThreshold();
}

size_t RegionSpaceMS::GetHotFreeBytes() const
{
    if (msRegionManager_ == nullptr) {
        return 0;
    }
    return msRegionManager_->GetHotFreeBytes();
}

size_t RegionSpaceMS::GetHotFreeBytesReference() const
{
    if (msRegionManager_ == nullptr) {
        return 0;
    }
    return msRegionManager_->GetHotFreeBytesReference();
}

bool RegionSpaceMS::HasEmptySweptRegions() const
{
    return msCentralPool_ != nullptr && msCentralPool_->GetEmptySweptRegionCount() > 0;
}

size_t RegionSpaceMS::FlushAllEmptySweptToHotFree()
{
    if (msCentralPool_ == nullptr) {
        return 0;
    }
    return msCentralPool_->ReclaimEmptyRegion(std::numeric_limits<size_t>::max());
}

size_t RegionSpaceMS::FlushEmptySweptToHotFree(size_t maxRegions)
{
    if (msCentralPool_ == nullptr || maxRegions == 0) {
        return 0;
    }
    return msCentralPool_->ReclaimEmptyRegion(maxRegions);
}

size_t RegionSpaceMS::FlushAllLargeHotCache()
{
    if (msCentralPool_ == nullptr) {
        return 0;
    }
    return msCentralPool_->ReclaimLargeHotCache(std::numeric_limits<size_t>::max());
}

size_t RegionSpaceMS::FlushLargeHotCacheUpToBytes(size_t maxBytes)
{
    if (msCentralPool_ == nullptr || maxBytes == 0) {
        return 0;
    }
    return msCentralPool_->ReclaimLargeHotCache(maxBytes);
}

size_t RegionSpaceMS::FlushCentralCacheToThreshold(size_t thresholdBytes)
{
    if (msCentralPool_ == nullptr) {
        return 0;
    }

    size_t cacheBytes = GetCentralCacheBytes();
    if (cacheBytes <= thresholdBytes) {
        return 0;
    }

    size_t bytesToFlush = cacheBytes - thresholdBytes;
    size_t flushedBytes = 0;

    size_t largeCacheBytes = msCentralPool_->GetLargeHotCacheBytes();
    if (bytesToFlush != 0 && largeCacheBytes != 0) {
        size_t flushedLarge = (largeCacheBytes <= bytesToFlush)
            ? FlushAllLargeHotCache()
            : FlushLargeHotCacheUpToBytes(bytesToFlush);
        flushedBytes += flushedLarge;
        bytesToFlush = (bytesToFlush > flushedLarge) ? (bytesToFlush - flushedLarge) : 0;
    }

    if (bytesToFlush != 0) {
        size_t emptySweptBytes = msCentralPool_->GetEmptySweptRegionCount() * MS_REGION_SIZE;
        if (emptySweptBytes != 0) {
            size_t flushedEmpty = (emptySweptBytes <= bytesToFlush)
                ? FlushAllEmptySweptToHotFree()
                : FlushEmptySweptToHotFree((bytesToFlush + MS_REGION_SIZE - 1) / MS_REGION_SIZE);
            flushedBytes += flushedEmpty;
        }
    }

    return flushedBytes;
}

size_t RegionSpaceMS::BackgroundScavenge(size_t quantumBytes)
{
    if (msRegionManager_ == nullptr) {
        return 0;
    }
    return msRegionManager_->ScavengeFreeRegionsUpTo(quantumBytes);
}

size_t RegionSpaceMS::BackgroundShrinkOneSegment()
{
    if (msRegionManager_ == nullptr || msCentralPool_ == nullptr) {
        return 0;
    }
    return msRegionManager_->ShrinkOneIdleSegment(msCentralPool_);
}

bool RegionSpaceMS::HasPendingScavengeWork() const
{
    size_t scavengeThreshold = GetScavengeThresholdBytes();
    if (scavengeThreshold == 0) {
        return false;
    }
    return GetHeapRetainedBytesReference() > scavengeThreshold;
}

bool RegionSpaceMS::HasPendingShrinkWork() const
{
    if (msRegionManager_ == nullptr || msCentralPool_ == nullptr) {
        return false;
    }
    return msRegionManager_->HasPendingShrinkWork(msCentralPool_);
}

void RegionSpaceMS::MaybeNotifyScavenge()
{
    if (msRegionManager_ != nullptr) {
        msRegionManager_->MaybeNotifyScavenge();
    }
}

void RegionSpaceMS::MaybeNotifyShrink()
{
    if (msRegionManager_ != nullptr) {
        msRegionManager_->MaybeNotifyShrink();
    }
}

bool RegionSpaceMS::ForEachObj(const std::function<void(BaseObject*)>& visitor, bool safe) const
{
    (void)visitor;
    (void)safe;
    return true;
}

MAddress RegionSpaceMS::TryAllocateLargeObjectOnce(size_t objectBytes, size_t allocSize, ThreadLocalData* local,
                                                   AllocBufferMS* msBuffer)
{
    if (msBuffer == nullptr) {
        msBuffer = AllocBufferMS::GetOrCreateAllocBufferMS(local);
    }
    size_t unitCount = (allocSize + MRT_PAGE_SIZE - 1) / MRT_PAGE_SIZE;
    size_t regionBytes = ((unitCount * MRT_PAGE_SIZE) + MS_REGION_SIZE - 1)
                         / MS_REGION_SIZE * MS_REGION_SIZE;

    // Hot cache fast path: reuse a recently-freed large region whose physical
    // pages are still resident — avoids fresh TakeRegions work and eliminates
    // page-fault cycles for the common churn pattern.
    uintptr_t hotCacheStart = 0;
    if (msCentralPool_ != nullptr &&
        msCentralPool_->TryTakeLargeHotCache(regionBytes, hotCacheStart)) {
        MAddress addr = static_cast<MAddress>(hotCacheStart);
        // Zero-fill the region so the mutator sees a clean object.
        ::memset(reinterpret_cast<void*>(hotCacheStart), 0, regionBytes);
        RegisterLargeObjectMS(addr, objectBytes, regionBytes);
        if (msBuffer != nullptr) {
            msBuffer->AccumulatePacingDebt(allocSize);
            msBuffer->AccumulateSweepDebt(regionBytes / MS_REGION_SIZE, kNumSizeClasses);
        }
        return addr;
    }

    uintptr_t regionStart = 0;
    AllocBufferMS* msBuffer2 = ThreadLocal::GetAllocBufferMS();
    uint16_t homeShardId = (msBuffer2 != nullptr)
        ? msBuffer2->GetCachedHomeShardId()
        : static_cast<uint16_t>(static_cast<uint32_t>(GetTid()) % msRegionManager_->GetShardCount());
    MSRegionInfo* baseInfo = msRegionManager_->TakeRegionForLarge(unitCount, regionStart, homeShardId);
    if (baseInfo == nullptr) {
        LOG(RTLOG_ERROR, "[MS-GC] TryAllocateOnceMS LARGE ALLOC FAILED: "
            "allocSize=%zu, unitCount=%zu, activeMSRegionBytes=%zu, "
            "msRegionManager freeUnits=%u",
            allocSize, unitCount,
            msCentralPool_ ? msCentralPool_->GetActiveMSRegionCount() * MS_REGION_SIZE : 0,
            msRegionManager_->GetFreeUnitCount());
        return 0;
    }
    MAddress addr = static_cast<MAddress>(regionStart);
    // Large arrays rely on zero-filled content after only their header is initialized.
    // TakeRegionForLarge may return hot-free pages that were not scavenged by the OS.
    ::memset(reinterpret_cast<void*>(regionStart), 0, regionBytes);
    RegisterLargeObjectMS(addr, objectBytes, regionBytes);
    // Pacing debt: large objects bypass AllocBufferMS::Allocate, so we must
    // accumulate assist debt explicitly here to keep GC pacing correct.
    if (msBuffer != nullptr) {
        msBuffer->AccumulatePacingDebt(allocSize);
        msBuffer->AccumulateSweepDebt(regionBytes / MS_REGION_SIZE, kNumSizeClasses);
    }
    return addr;
}


MAddress RegionSpaceMS::TryAllocateRegularObjectOnce(size_t allocSize, ThreadLocalData* local, AllocBufferMS*& msBuffer)
{
    if (msBuffer == nullptr) {
        msBuffer = AllocBufferMS::GetOrCreateAllocBufferMS(local);
    }
    if (msBuffer != nullptr) {
        MAddress addr = msBuffer->Allocate(allocSize);
        if (LIKELY(addr != 0)) {
            return addr;
        }
    }
    LOG(RTLOG_ERROR, "[MS-GC] TryAllocateRegularObjectOnce FAILED: "
        "allocSize=%zu, msBuffer=%p",
        allocSize, static_cast<void*>(msBuffer));
    return 0;
}

size_t RegionSpaceMS::AllocatedBytes() const
{
    return GetUsedPageSize();
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
    size_t configuredMaxVirtualHeapSize = CangjieRuntime::GetMSMaxVirtualHeapSizeKB() * KB;

    size_t maxVirtualHeapSize = MSRegionManager::ComputeMaxVirtualHeapSize(heapSize, configuredMaxVirtualHeapSize);
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
    msRegionManager_->Initialize(metadata, maxVirtualHeapSize);

    reservedStart = msRegionManager_->GetHeapStart();
    reservedEnd = msRegionManager_->GetHeapEnd();
    MAddress reservedLimit = msRegionManager_->GetMaxHeapEnd();

#if defined(MRT_DUMP_ADDRESS)
    VLOG(REPORT, "region metadata@%zx, heap @[0x%zx+%zu, 0x%zx)", metadata, reservedStart, reservedEnd - reservedStart,
         reservedEnd);
#endif
    Heap::OnHeapCreated(reservedStart, reservedLimit);
    Heap::OnHeapExtended(reservedEnd);

    SizeClassInfo::Init();
    size_t maxVirtualRegionCount = msRegionManager_->GetMaxVirtualRegionCount();
    MSRegionInfoArray::Init(reservedStart, maxVirtualRegionCount, msRegionManager_->GetArrayBase());

    msCentralPool_ = new (std::nothrow) CentralRegionPool(msRegionManager_);
    CHECK_DETAIL(msCentralPool_ != nullptr, "failed to create CentralRegionPool");

    VLOG(REPORT, "[MS-GC] MS subsystem initialized: committedRegions=%zu, maxVirtualRegions=%zu, "
         "committedHeapRange=[0x%zx, 0x%zx), reservedHeapRange=[0x%zx, 0x%zx)",
         msRegionManager_->GetMaxRegionCount(), maxVirtualRegionCount,
         reservedStart, reservedEnd, reservedStart, reservedLimit);
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
    // MSGC manages RAW_POINTER_OBJECT in the same central Region lifecycle.
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

void RegionSpaceMS::RegisterLargeObjectMS(MAddress addr, size_t objectBytes, size_t regionBytes)
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

    baseInfo->InitForLargeObject(start, end, objectBytes);
    baseInfo->reserved = static_cast<uint64_t>(baseIdx);
    MSRegionInfoArray::SetState(baseInfo, MSRegionLocation::LARGE_ACTIVE, 0);

    for (size_t i = 1; i < spanCount; i++) {
        MSRegionInfo* spanInfo = MSRegionInfoArray::GetRegionInfoByIdx(baseIdx + i);
        spanInfo->regionType = MSRegionType::MS_REGION_LARGE;
        spanInfo->markBitmap = nullptr;
        spanInfo->allocBitmap = nullptr;
        spanInfo->totalSlotCount = 0;
        spanInfo->freeSlotCount = 0;
        spanInfo->flags_ = 0;
        spanInfo->reserved = static_cast<uint64_t>(baseIdx);
        MSRegionInfoArray::SetState(spanInfo, MSRegionLocation::LARGE_ACTIVE, 0);
    }

    baseInfo->AtomicSetMarked();

    // BLACK allocation for large objects during marking: the hybrid write
    // barrier (SATB + insertion) ensures children are independently
    // discovered when fields are written. No need to enqueue.

    if (msCentralPool_ != nullptr) {
        msCentralPool_->RegisterLargeRegion(baseInfo);
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
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
