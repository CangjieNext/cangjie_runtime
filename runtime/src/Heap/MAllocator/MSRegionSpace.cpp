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
#include "Heap/MCollector/MSMemoryLimitPolicy.h"
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
    (void)allocType;
    return AllocateInternal(
        size, MSScanClass::HAS_REF, ThreadLocal::GetThreadLocalData());
}

MAddress RegionSpaceMS::Allocate(size_t size, MSScanClass scanClass, ThreadLocalData* local)
{
    return AllocateInternal(size, scanClass, local);
}

MAddress RegionSpaceMS::AllocateInternal(
    size_t size, MSScanClass scanClass, ThreadLocalData* local)
{
    size_t allocSize = ToAllocSize(size);
    if (UNLIKELY(SizeClassInfo::IsLargeObjectSize(allocSize))) {
        return AllocateLargeObject(size, allocSize, scanClass, local);
    }

    return AllocateRegularObject(size, allocSize, scanClass, local);
}

MAddress RegionSpaceMS::AllocateLargeObject(
    size_t size, size_t allocSize, MSScanClass scanClass, ThreadLocalData* local)
{
    AllocBufferMS* msBuffer = local != nullptr ? local->msAllocBuffer : AllocBufferMS::GetAllocBufferMS();
    bool allowEmergency = !IsGcThread();
    uintptr_t internalAddr = TryMSAllocationWithEmergencyRetry(
        allowEmergency,
        [this, size, allocSize, scanClass, local, msBuffer]() {
            return TryAllocateLargeObjectOnce(size, allocSize, scanClass, local, msBuffer);
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

MAddress RegionSpaceMS::AllocateRegularObject(
    size_t size, size_t allocSize, MSScanClass scanClass, ThreadLocalData* local)
{
    AllocBufferMS* msBuffer = local != nullptr ? local->msAllocBuffer : AllocBufferMS::GetAllocBufferMS();
    bool allowEmergency = !IsGcThread();
    uintptr_t internalAddr = TryMSAllocationWithEmergencyRetry(
        allowEmergency,
        [this, allocSize, scanClass, local, &msBuffer]() {
            return TryAllocateRegularObjectOnce(allocSize, scanClass, local, msBuffer);
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
    return msCentralPool_->GetEmptySweptRegionCount() * MS_REGION_SIZE;
}

size_t RegionSpaceMS::GetHeapRetainedBytes() const
{
    if (msCentralPool_ == nullptr || msRegionManager_ == nullptr) {
        return 0;
    }
    size_t active = msCentralPool_->GetActiveMSRegionCount() * MS_REGION_SIZE;
    size_t empty = msCentralPool_->GetEmptySweptRegionCount() * MS_REGION_SIZE;
    size_t hotFree = msRegionManager_->GetHotFreeBytes();
    return active + empty + hotFree;
}

size_t RegionSpaceMS::GetHeapRetainedBytesReference() const
{
    if (msCentralPool_ == nullptr || msRegionManager_ == nullptr) {
        return 0;
    }
    size_t active = GetHeapInUseBytes();
    size_t empty = msCentralPool_->GetEmptySweptRegionCount() * MS_REGION_SIZE;
    size_t hotFree = msRegionManager_->GetHotFreeBytesReference();
    return active + empty + hotFree;
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
    if (cacheBytes <= bytesToFlush) {
        return FlushAllEmptySweptToHotFree();
    }
    return FlushEmptySweptToHotFree((bytesToFlush + MS_REGION_SIZE - 1) / MS_REGION_SIZE);
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
    return GetScavengeReleaseTargetBytes() != 0;
}

size_t RegionSpaceMS::GetScavengeReleaseTargetBytes() const
{
    if (msCentralPool_ == nullptr || msRegionManager_ == nullptr) {
        return 0;
    }
    size_t scavengeThreshold = GetScavengeThresholdBytes();
    if (scavengeThreshold == 0) {
        return 0;
    }

    size_t inUseBytes = GetHeapInUseBytes();
    size_t reclaimableBytes =
        msCentralPool_->GetEmptySweptRegionCount() * MS_REGION_SIZE +
        msRegionManager_->GetHotFreeBytes();
    size_t normalTarget = std::max(scavengeThreshold, inUseBytes);
    size_t effectiveTarget = ResolveMSScavengeTarget(
        normalTarget, CangjieRuntime::GetMSSoftHeapLimitBytes());
    size_t retainedBytes = inUseBytes + reclaimableBytes;
    return ComputeMSReleaseGap(retainedBytes, reclaimableBytes, effectiveTarget);
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

bool RegionSpaceMS::ForEachObj(const std::function<void(BaseObject*)>& visitor, bool safe) const
{
    (void)visitor;
    (void)safe;
    return true;
}

MAddress RegionSpaceMS::TryAllocateLargeObjectOnce(
    size_t objectBytes, size_t allocSize, MSScanClass scanClass,
    ThreadLocalData* local, AllocBufferMS* msBuffer)
{
    if (msBuffer == nullptr) {
        msBuffer = AllocBufferMS::GetOrCreateAllocBufferMS(local);
    }
    size_t unitCount = (allocSize + MRT_PAGE_SIZE - 1) / MRT_PAGE_SIZE;
    size_t regionBytes = ((unitCount * MRT_PAGE_SIZE) + MS_REGION_SIZE - 1)
                         / MS_REGION_SIZE * MS_REGION_SIZE;

    uintptr_t regionStart = 0;
    uint16_t homeShardId = (msBuffer != nullptr)
        ? msBuffer->GetCachedHomeShardId()
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
    RegisterLargeObjectMS(addr, objectBytes, regionBytes, scanClass);
    // Pacing debt: large objects bypass AllocBufferMS::Allocate, so we must
    // accumulate assist debt explicitly here to keep GC pacing correct.
    if (msBuffer != nullptr) {
        msBuffer->AccumulatePacingDebt(allocSize);
        msBuffer->AccumulateSweepDebt(regionBytes / MS_REGION_SIZE, kNumSizeClasses);
    }
    return addr;
}


MAddress RegionSpaceMS::TryAllocateRegularObjectOnce(
    size_t allocSize, MSScanClass scanClass, ThreadLocalData* local, AllocBufferMS*& msBuffer)
{
    if (msBuffer == nullptr) {
        msBuffer = AllocBufferMS::GetOrCreateAllocBufferMS(local);
    }
    if (msBuffer != nullptr) {
        MAddress addr = msBuffer->Allocate(allocSize, scanClass);
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
            if (msInfo->IsLargeRegionEntry()) {
                MSRegionInfo* base = MSRegionInfoArray::ResolveLargeBaseInfo(msInfo);
                return base != nullptr && base->IsMarked();
            }
            if (msInfo->GetRegionKind() == MSRegionKind::SMALL &&
                msInfo->GetTotalSlotCount() > 0) {
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
    CHECK_DETAIL(vmHeapParam.heapSize <= std::numeric_limits<size_t>::max() / 1024,
                 "[MS-GC] initial heap KB exceeds addressable byte size: %zu", vmHeapParam.heapSize);
    size_t heapSize = vmHeapParam.heapSize * 1024;
    size_t configuredMaxVirtualHeapSizeKB = CangjieRuntime::GetMSMaxVirtualHeapSizeKB();
    CHECK_DETAIL(configuredMaxVirtualHeapSizeKB <= kMSMaxVirtualHeapSizeKB,
                 "[MS-GC] max virtual heap KB exceeds addressable byte size: %zu",
                 configuredMaxVirtualHeapSizeKB);
    size_t configuredMaxVirtualHeapSize = configuredMaxVirtualHeapSizeKB * KB;

    size_t requestedMaxVirtualHeapSize =
        std::max(heapSize, configuredMaxVirtualHeapSize);
    size_t maxVirtualHeapSize = 0;
    size_t metadataSize = 0;
    size_t totalSize = 0;
    CHECK_DETAIL(MSRegionManager::TryComputeVirtualReserveLayout(
                     requestedMaxVirtualHeapSize, maxVirtualHeapSize, metadataSize, totalSize),
                 "[MS-GC] virtual reserve layout is not addressable: initial=%zu configured=%zu",
                 heapSize, configuredMaxVirtualHeapSize);
    (void)metadataSize;
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
    // MSGC compiler fast paths load the dedicated msAllocBuffer field. The
    // CopyGC `buffer` field remains in ThreadLocalData for the other build
    // mode, but MSGC never aliases an AllocBufferMS through that type.
    (void)AllocBufferMS::GetOrCreateAllocBufferMS();
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

void RegionSpaceMS::RegisterLargeObjectMS(
    MAddress addr, size_t objectBytes, size_t regionBytes, MSScanClass scanClass)
{
    (void)scanClass;
    uintptr_t start = static_cast<uintptr_t>(addr);
    uintptr_t end = start + regionBytes;

    MSRegionInfo* baseInfo = MSRegionInfoArray::GetRegionInfo(start);
    size_t baseIdx = MSRegionInfoArray::GetRegionIdx(baseInfo);

    size_t spanCount = (regionBytes + MS_REGION_SIZE - 1) / MS_REGION_SIZE;
    for (size_t i = 0; i < spanCount; i++) {
        MSRegionInfo* entry = MSRegionInfoArray::GetRegionInfoByIdx(baseIdx + i);
        if (!entry->IsFreeRegion() &&
            entry->GetRegionKind() != MSRegionKind::LARGE) {
            LOG(RTLOG_FATAL, "[MS-GC] RegisterLargeObjectMS: span[%zu/%zu] (idx=%zu) has "
                "non-free regionType=%d! allocPtr=0x%zx, regionEnd=0x%zx, "
                "allocBitmap=%p, sizeClass=%u, freeSlots=%u, totalSlots=%u, "
                "allocMode=%u, collectEpoch=%u, centralNext=%d, markTaskNext=%d, reserved=0x%llx. "
                "Large: start=0x%zx, end=0x%zx, regionBytes=%zu",
                i, spanCount, baseIdx + i,
                (int)entry->GetRegionType(), entry->allocPtr, entry->regionEnd,
                (void*)entry->GetAllocBitmap(), (unsigned)entry->sizeClass,
                (unsigned)entry->CountFreeSlots(), (unsigned)entry->GetTotalSlotCount(),
                static_cast<unsigned>(entry->GetAllocMode()),
                static_cast<unsigned>(entry->collectEpoch),
                static_cast<int32_t>(entry->GetCentralNextRegionIdx()),
                static_cast<int32_t>(entry->GetMarkTaskNextRegionIdx()),
                (unsigned long long)entry->allocSummary,
                start, end, regionBytes);
        }
    }

    baseInfo->InitForLargeObject(start, end, objectBytes, scanClass);
    for (size_t i = 1; i < spanCount; i++) {
        MSRegionInfo* spanInfo = MSRegionInfoArray::GetRegionInfoByIdx(baseIdx + i);
        spanInfo->allocPtr = 0;
        spanInfo->regionEnd = 0;
        spanInfo->SetCentralNextRegionIdx(MSRegionInfo::INVALID_REGION_IDX);
        spanInfo->SetMarkTaskNextRegionIdx(MSRegionInfo::INVALID_REGION_IDX);
        spanInfo->largeObjectBytes = 0;
        spanInfo->collectEpoch = MSRegionInfo::NO_COLLECT_EPOCH;
        spanInfo->control = 0;
        spanInfo->allocSummary = 0;
        spanInfo->pendingWordSummary = 0;
        spanInfo->reserved32 = 0;
        spanInfo->allocMode = static_cast<uint8_t>(MSAllocMode::NONE);
        spanInfo->reserved8 = 0;
        spanInfo->sizeClass = MSRegionInfo::INVALID_SIZE_CLASS;
        spanInfo->layout = static_cast<uint8_t>(MSRegionKind::SPAN);
        spanInfo->SetSpanBaseRegionIdx(static_cast<uint32_t>(baseIdx));
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
