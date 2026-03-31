// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/RegionSpace.h"

#include "Collector/Collector.h"
#include "Collector/CollectorResources.h"
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"

namespace MapleRuntime {

MAddress RegionSpace::TryAllocateOnce(size_t allocSize, AllocType allocType)
{
    if (UNLIKELY(allocType == AllocType::PINNED_OBJECT)) {
        return regionManager.AllocPinned(allocSize);
    }
    if (UNLIKELY(allocSize >= regionManager.GetLargeObjectThreshold())) {
        return regionManager.AllocLarge(allocSize);
    }
    AllocBuffer* allocBuffer = AllocBuffer::GetOrCreateAllocBuffer();
    return allocBuffer->Allocate(allocSize, allocType);
}

bool RegionSpace::ShouldRetryAllocation(size_t& tryTimes, size_t size) const
{
    if (!IsRuntimeThread() && tryTimes <= static_cast<size_t>(TryAllocationThreshold::RESCHEDULE)) {
        CJThreadResched(); // reschedule this thread for throughput.
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

MAddress RegionSpace::Allocate(size_t size, AllocType allocType)
{
    size_t tryTimes = 0;
    uintptr_t internalAddr = 0;
    size_t allocSize = ToAllocSize(size);
    do {
        tryTimes++;
        internalAddr = TryAllocateOnce(allocSize, allocType);
        if (LIKELY(internalAddr != 0)) {
            break;
        }
        if (IsGcThread()) {
            return 0; // it means gc doesn't have enough space to move this object.
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

// In CopyGC mode: returns region-level allocated bytes via RegionManager.
size_t RegionSpace::AllocatedBytes() const
{
    return regionManager.GetAllocatedSize();
}

bool Allocator::IsMarkedObject(const BaseObject* obj)
{
    RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    return regionInfo->IsMarkedObject(obj);
}

bool Allocator::MarkObject(const BaseObject* obj)
{
    RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    return regionInfo->MarkObject(obj);
}

bool Allocator::ShouldEnqueue(const BaseObject* obj)
{
    RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    if (regionInfo->IsTraceRegion()) {
        return false;
    }
    size_t offset = regionInfo->GetAddressOffset(reinterpret_cast<MAddress>(obj));
    if (regionInfo->IsMarkedObject(offset)) {
        return false;
    }
    return !regionInfo->EnqueueObject(obj, offset);
}

bool Allocator::IsResurrectedObject([[maybe_unused]] const BaseObject* obj)
{
    RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    return regionInfo->IsResurrectedObject(obj);
}

void Allocator::EnsureThreadLocalBuffer()
{
    (void)AllocBuffer::GetOrCreateAllocBuffer();
}

void Allocator::CommitRawPointerRegions()
{
    AllocBuffer* buffer = AllocBuffer::GetAllocBuffer();
    if (buffer != nullptr) {
        buffer->CommitRawPointerRegions();
    }
}

void Allocator::PushRootToBuffer(BaseObject* root)
{
    AllocBuffer* buffer = AllocBuffer::GetOrCreateAllocBuffer();
    buffer->PushRoot(root);
}

void RegionSpace::Init(const HeapParam& vmHeapParam)
{
    MemMap::Option opt = MemMap::DEFAULT_OPTIONS;
    opt.tag = "cangjie_heap";
    size_t heapSize = vmHeapParam.heapSize * 1024;

    size_t totalSize = RegionManager::GetHeapMemorySize(heapSize);

    size_t unitNum = RegionManager::GetHeapUnitCount(heapSize);
#if defined(CANGJIE_ASAN_SUPPORT)
    // asan's memory alias technique needs a shareable page
    opt.flags &= ~MAP_PRIVATE;
    opt.flags |= MAP_SHARED;
    DLOG(SANITIZER, "mmap flags set to 0x%x", opt.flags);
#endif
    // this must succeed otherwise it won't return
    map = MemMap::MapMemory(totalSize, totalSize, opt);
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
    Sanitizer::OnHeapAllocated(map->GetBaseAddr(), map->GetMappedSize());
#endif

    Logger::GetLogger().SetMinimumLogLevel(CangjieRuntime::GetLogParam().logLevel);
    MAddress metadata = reinterpret_cast<MAddress>(map->GetBaseAddr());

    regionManager.Initialize(unitNum, metadata);
    reservedStart = regionManager.GetRegionHeapStart();
    reservedEnd = reinterpret_cast<MAddress>(map->GetMappedEndAddr());

#if defined(MRT_DUMP_ADDRESS)
    VLOG(REPORT, "region metadata@%zx, heap @[0x%zx+%zu, 0x%zx)", metadata, reservedStart, reservedEnd - reservedStart,
         reservedEnd);
#endif
    Heap::OnHeapCreated(reservedStart);
    Heap::OnHeapExtended(reservedEnd);
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
bool RegionSpace::IsHeapObject(MAddress addr) const
{
    return IsHeapAddress(addr);
}
#endif
void RegionSpace::FeedHungryBuffers()
{
    ScopedObjectAccess soa;
    AllocBufferManager::HungryBuffers hungryBuffers;
    allocBufferManager_->SwapHungryBuffers(hungryBuffers);
    RegionInfo* region = nullptr;
    for (auto* buffer : hungryBuffers) {
        if (buffer->GetPreparedRegion() != nullptr) { continue; }
        if (region == nullptr) {
            region = regionManager.AllocateThreadLocalRegion(true);
            if (region == nullptr) { return; }
        }
        if (buffer->SetPreparedRegion(region)) {
            region = nullptr;
        }
    }
    if (region != nullptr) {
        // The region is inserted in thread-local region list when allocated, we need to remove it from the list.
        regionManager.RemoveThreadLocalRegion(region);
        regionManager.CollectRegion(region);
    }
}

// ============================================================================
// CopyGC-only methods (declarations in RegionSpace.h, excluded from MS build via CMake)
// ============================================================================

size_t RegionSpace::RefineFromSpace()
{
    MRT_PHASE_TIMER("ExemptFromRegions");
    return regionManager.ExemptFromRegions();
}

BaseObject* RegionSpace::RouteObject(BaseObject* fromObj) { return regionManager.RouteObject(fromObj); }

void RegionSpace::PrepareFromSpace() { regionManager.PrepareFromRegionList(); }

void RegionSpace::ClearAllLiveInfo() { regionManager.ClearAllLiveInfo(); }

void RegionSpace::ForwardFromSpace(GCThreadPool* threadPool)
{
    MRT_PHASE_TIMER("ForwardFromRegions");
    regionManager.ForwardFromRegions(threadPool);
}

size_t RegionSpace::CollectLargeGarbage() { return regionManager.CollectLargeGarbage(); }

size_t RegionSpace::CollectPinnedGarbage() { return regionManager.CollectPinnedGarbage(); }

void RegionSpace::CollectFromSpaceGarbage()
{
    regionManager.CollectFromSpaceGarbage();
    regionManager.ReassembleFromSpace();
}

void RegionSpace::AssembleGarbageCandidates(bool collectAll)
{
    regionManager.AssembleSmallGarbageCandidates();
    regionManager.AssemblePinnedGarbageCandidates(collectAll);
    regionManager.AssembleLargeGarbageCandidates();
}

void RegionSpace::DumpRegionStats(const char* msg) const { regionManager.DumpRegionStats(msg); }

void RegionSpace::CountLiveObject(const BaseObject* obj) { regionManager.CountLiveObject(obj); }

void RegionSpace::PrepareTrace() { regionManager.PrepareTrace(); }

void RegionSpace::AddRawPointerObject(BaseObject* obj) { regionManager.AddRawPointerObject(obj); }

void RegionSpace::RemoveRawPointerObject(BaseObject* obj) { regionManager.RemoveRawPointerObject(obj); }

} // namespace MapleRuntime
