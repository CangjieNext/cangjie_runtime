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

#ifdef MRT_USE_MARK_SWEEP
#include "Allocator/AllocBufferMS.h"
#include "Allocator/CentralRegionPool.h"
#include "Allocator/MSRegionInfo.h"
#include "Allocator/MSRegionManager.h"
#include "Allocator/SizeClass.h"
#include "Allocator/LargeObjectSpace.h"
#include "Mutator/ThreadLocal.h"
#endif

namespace MapleRuntime {

#ifdef MRT_USE_MARK_SWEEP
// Mark-Sweep allocation path — handles all object sizes:
//   Tier 2 (huge, >4MB):  independent mmap via LargeObjectSpace
//   Tier 1 (large):       MSRegionManager units + MSRegionInfo registration
//   Normal (small/medium): per-thread AllocBufferMS → CentralRegionPool
// In MS mode, PINNED_OBJECT is equivalent to MOVEABLE (nothing moves),
// so allocType is not needed here.
MAddress RegionSpace::TryAllocateOnceMS(size_t allocSize)
{
    // Large / huge object path
    // Use kLargeObjectThreshold (16KB, max size class) as the cutoff, NOT
    // MS_REGION_SIZE (32KB).  Objects between 16KB and 32KB fall into a gap:
    // SizeToClass() overflows its lookup table, AllocBufferMS::Allocate()
    // rejects them (cls >= kNumSizeClasses → return 0), and they never reach
    // the large object path — causing infinite retry → OOM.
    if (UNLIKELY(allocSize >= kLargeObjectThreshold)) {
        // Tier 2: huge objects (>4MB) go to independent mmap
        if (allocSize > kHugeObjectThreshold && msLargeObjectSpace_ != nullptr) {
            return msLargeObjectSpace_->Allocate(allocSize);
        }
        // Tier 1: large objects — allocate via MSRegionManager directly.
        // One-level: MSRegionManager returns MSRegionInfo* + regionStart.
        size_t unitCount = (allocSize + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;
        uintptr_t regionStart = 0;
        MSRegionInfo* baseInfo = msRegionManager_->TakeRegionForLarge(unitCount, regionStart);
        if (baseInfo == nullptr && msCentralPool_ != nullptr) {
            // No free physical pages.  Force-sweep all CentralRegionPool small
            // regions so that completely-empty regions return their pages to
            // MSRegionManager.  This handles the case where only large objects are
            // being allocated and lazy sweep never runs.
            size_t beforeBytes = msCentralPool_->GetTotalMSRegionBytes();
            msCentralPool_->ForceFinishSweep();
            size_t afterBytes = msCentralPool_->GetTotalMSRegionBytes();
            LOG(RTLOG_ERROR, "[MS-GC] TryAllocateOnceMS large fallback: "
                "allocSize=%zu, unitCount=%zu, "
                "totalMSRegionBytes before=%zu after=%zu (freed=%zd), "
                "msRegionManager freeUnits=%u",
                allocSize, unitCount,
                beforeBytes, afterBytes, (ssize_t)(beforeBytes - afterBytes),
                msRegionManager_->GetFreeUnitCount());
            baseInfo = msRegionManager_->TakeRegionForLarge(unitCount, regionStart);
        }
        if (baseInfo == nullptr) {
            // ForceFinishSweep was not enough.  Request a full GC to mark+sweep
            // all garbage (including large objects), then retry once.
            if (!Heap::GetHeap().IsGcStarted()) {
                Heap::GetHeap().GetCollector().RequestGC(GC_REASON_OOM, false);
            }
            {
                ScopedEnterSaferegion enterSaferegion(false);
                Heap::GetHeap().GetCollectorResources().WaitForGCFinish();
            }
            baseInfo = msRegionManager_->TakeRegionForLarge(unitCount, regionStart);
        }
        if (baseInfo == nullptr) {
            LOG(RTLOG_ERROR, "[MS-GC] TryAllocateOnceMS LARGE ALLOC FAILED after GC: "
                "allocSize=%zu, unitCount=%zu, totalMSRegionBytes=%zu, "
                "msRegionManager freeUnits=%u",
                allocSize, unitCount,
                msCentralPool_ ? msCentralPool_->GetTotalMSRegionBytes() : 0,
                msRegionManager_->GetFreeUnitCount());
            return 0;
        }
        // Alloc from the raw region memory.
        // regionBytes must be rounded up to MS_REGION_SIZE to match the
        // aligned allocation inside TakeRegionForLarge — otherwise
        // InitForLargeObject records a smaller regionEnd, spanCount is
        // under-counted, and trailing MSRegionInfo entries stay un-initialized.
        size_t regionBytes = ((unitCount * RegionInfo::UNIT_SIZE) + MS_REGION_SIZE - 1)
                             / MS_REGION_SIZE * MS_REGION_SIZE;
        MAddress addr = static_cast<MAddress>(regionStart);
        RegisterLargeObjectMS(addr, regionBytes);
        return addr;
    }

    // Normal (small/medium) object path: per-thread AllocBufferMS
    AllocBufferMS* msBuffer = GetOrCreateAllocBufferMS();
    if (msBuffer != nullptr) {
        MAddress addr = msBuffer->Allocate(allocSize);
        if (LIKELY(addr != 0)) {
            return addr;
        }
    }
    // Allocation failed, let retry logic handle GC/OOM
    return 0;
}
#endif

MAddress RegionSpace::TryAllocateOnce(size_t allocSize, AllocType allocType)
{
#ifdef MRT_USE_MARK_SWEEP
    return TryAllocateOnceMS(allocSize);
#else
    if (UNLIKELY(allocType == AllocType::PINNED_OBJECT)) {
        return regionManager.AllocPinned(allocSize);
    }
    if (UNLIKELY(allocSize >= regionManager.GetLargeObjectThreshold())) {
        return regionManager.AllocLarge(allocSize);
    }
    AllocBuffer* allocBuffer = AllocBuffer::GetOrCreateAllocBuffer();
    return allocBuffer->Allocate(allocSize, allocType);
#endif
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

size_t RegionSpace::AllocatedBytes() const
{
#ifdef MRT_USE_MARK_SWEEP
    if (msCentralPool_ != nullptr) {
        return msCentralPool_->GetTotalMSRegionBytes();
    }
#endif
    return regionManager.GetAllocatedSize();
}

bool RegionSpace::IsMarkedObject(const BaseObject* obj)
{
#ifdef MRT_USE_MARK_SWEEP
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (MSRegionInfoArray::IsHeapAddress(addr)) {
        MSRegionInfo* msInfo = MSRegionInfoArray::GetRegionInfoValidated(addr);
        if (msInfo != nullptr) {
            // Large objects: check isMarked on base entry
            if (msInfo->regionType == MSRegionType::MS_REGION_LARGE) {
                MSRegionInfo* base = MSRegionInfoArray::GetRegionInfoByIdx(
                    static_cast<size_t>(msInfo->reserved));
                return base->isMarked != 0;
            }
            // Small/medium: check mark bitmap
            if (msInfo->markBitmap != nullptr && msInfo->totalSlotCount > 0) {
                return msInfo->GetMarkBit(addr);
            }
        }
    }
    // In MS mode, if not found in MSRegionInfoArray, treat as unmarked.
    // Do NOT fall through to CopyGC RegionInfo — UnitInfo mirror is not
    // initialized in MS mode.
    return false;
#else
    // Fall back to CopyGC RegionInfo (handles LOS large regions via isMarked flag)
    RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    return regionInfo->IsMarkedObject(obj);
#endif
}

void RegionSpace::Init(const HeapParam& vmHeapParam)
{
    MemMap::Option opt = MemMap::DEFAULT_OPTIONS;
    opt.tag = "cangjie_heap";
    size_t heapSize = vmHeapParam.heapSize * 1024;

#ifdef MRT_USE_MARK_SWEEP
    // MS mode: use MSRegionManager's layout (MSRegionInfo[] at mmap front).
    size_t totalSize = MSRegionManager::GetHeapMemorySize(heapSize);
#else
    size_t totalSize = RegionManager::GetHeapMemorySize(heapSize);
#endif

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

#ifdef MRT_USE_MARK_SWEEP
    // MS mode: initialize MSRegionManager which owns the MSRegionInfo[] at mmap front.
    msRegionManager_ = new (std::nothrow) MSRegionManager();
    CHECK_DETAIL(msRegionManager_ != nullptr, "failed to create MSRegionManager");
    msRegionManager_->Initialize(heapSize, metadata);

    reservedStart = msRegionManager_->GetHeapStart();
    reservedEnd = msRegionManager_->GetHeapEnd();

    // Set RegionInfo static members (UNIT_SIZE, heapStartAddress, totalUnitCount)
    // so that legacy global lookups don't crash. We do NOT call
    // regionManager.Initialize() because the CopyGC UnitInfo mirror layout
    // would overlap with MSRegionInfo[] at the mmap front.
    RegionInfo::Initialize(msRegionManager_->GetMaxUnitCount(), reservedStart);
#else
    regionManager.Initialize(unitNum, metadata);
    reservedStart = regionManager.GetRegionHeapStart();
    reservedEnd = reinterpret_cast<MAddress>(map->GetMappedEndAddr());
#endif

#if defined(MRT_DUMP_ADDRESS)
    VLOG(REPORT, "region metadata@%zx, heap @[0x%zx+%zu, 0x%zx)", metadata, reservedStart, reservedEnd - reservedStart,
         reservedEnd);
#endif
    Heap::OnHeapCreated(reservedStart);
    Heap::OnHeapExtended(reservedEnd);

#ifdef MRT_USE_MARK_SWEEP
    // Initialize the Mark-Sweep allocation subsystem
    // 0. SizeClassInfo lookup tables (must be first — needed by all MS allocators)
    SizeClassInfo::Init();

    // 1. MSRegionInfoArray: use the mmap-front array from MSRegionManager (no malloc).
    size_t heapBytes = reservedEnd - reservedStart;
    size_t maxMSRegionCount = heapBytes / MS_REGION_SIZE;
    MSRegionInfoArray::Init(reservedStart, maxMSRegionCount, msRegionManager_->GetArrayBase());

    // 2. CentralRegionPool: per-size-class Region pools (now uses MSRegionManager*)
    msCentralPool_ = new (std::nothrow) CentralRegionPool(msRegionManager_);
    CHECK_DETAIL(msCentralPool_ != nullptr, "failed to create CentralRegionPool");

    // 3. AllocBufferMS is now per-thread (lazily created via GetOrCreateAllocBufferMS).
    //    No global instance needed.

    // 4. LargeObjectSpace: independent mmap for huge objects (>4MB)
    msLargeObjectSpace_ = new (std::nothrow) LargeObjectSpace();
    CHECK_DETAIL(msLargeObjectSpace_ != nullptr, "failed to create LargeObjectSpace");

    VLOG(REPORT, "[MS-GC] MS subsystem initialized: maxRegions=%zu, heapRange=[0x%zx, 0x%zx)",
         maxMSRegionCount, reservedStart, reservedEnd);
#endif
}

#ifdef MRT_USE_MARK_SWEEP
// MS-mode MarkObject: uses MSRegionInfo mark bitmap / isMarked flag
// Returns true if the object was ALREADY marked, false if NEWLY marked.
bool RegionSpace::MarkObject(const BaseObject* obj)
{
    MAddress addr = reinterpret_cast<MAddress>(obj);

    // Non-heap addresses: check LOS (Tier 2 huge objects) before rejecting
    if (!MSRegionInfoArray::IsHeapAddress(addr)) {
        // Try LOS — these objects are mmap'd outside the contiguous heap
        RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
        LargeObjectSpace* los = space.GetLargeObjectSpace();
        if (los != nullptr && los->MarkObject(static_cast<uintptr_t>(addr))) {
            return false;  // newly marked in LOS
        }
        return true;  // not managed — treat as already marked
    }

    MSRegionInfo* msInfo = MSRegionInfoArray::GetRegionInfoValidated(addr);
    if (msInfo == nullptr) {
        return true;
    }

    // Large region: use isMarked flag on base MSRegionInfo
    if (msInfo->regionType == MSRegionType::MS_REGION_LARGE) {
        // Resolve to base MSRegionInfo (handles interior pointers across spans)
        MSRegionInfo* base = MSRegionInfoArray::GetRegionInfoByIdx(
            static_cast<size_t>(msInfo->reserved));
        if (base->isMarked) {
            return true;  // already marked
        }
        base->isMarked = 1;
        return false;  // newly marked
    }

    // Small/medium region: use mark bitmap
    if (msInfo->markBitmap != nullptr && msInfo->totalSlotCount > 0) {
        return msInfo->TestAndSetMarkBit(addr);  // true = already set
    }

    // MSRegionInfo not yet initialized (edge case) → treat as already marked
    return true;
}

void RegionSpace::RegisterLargeObjectMS(MAddress addr, size_t regionBytes)
{
    uintptr_t start = static_cast<uintptr_t>(addr);
    uintptr_t end = start + regionBytes;

    // Initialize the base MSRegionInfo at the object's start address
    MSRegionInfo* baseInfo = MSRegionInfoArray::GetRegionInfo(start);
    size_t baseIdx = MSRegionInfoArray::GetRegionIdx(baseInfo);

    // Debug: check ALL span entries BEFORE overwriting — detect if any are in active use
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
    // Base points to itself
    baseInfo->reserved = static_cast<uint64_t>(baseIdx);

    // Initialize ALL spanning MSRegionInfo entries for interior pointer safety
    for (size_t i = 1; i < spanCount; i++) {
        MSRegionInfo* spanInfo = MSRegionInfoArray::GetRegionInfoByIdx(baseIdx + i);
        spanInfo->regionType = MSRegionType::MS_REGION_LARGE;
        spanInfo->markBitmap = nullptr;
        spanInfo->allocBitmap = nullptr;
        spanInfo->totalSlotCount = 0;
        spanInfo->freeSlotCount = 0;
        spanInfo->isMarked = 0;
        // Back-pointer to base MSRegionInfo index
        spanInfo->reserved = static_cast<uint64_t>(baseIdx);
    }

    // Always mark newly allocated large objects as live.
    // ClearLargeMarks() will reset isMarked=0 at the start of each GC cycle
    // (inside STW), and the mark phase will re-mark live objects.
    // Previously this was conditional on GC phase, causing large objects
    // allocated during IDLE to have isMarked=0 — ForceFinishSweep's
    // SweepLargeRegions would then incorrectly free them as "dead".
    baseInfo->isMarked = 1;

    // Track in CentralRegionPool for GC sweep iteration and byte accounting
    if (msCentralPool_ != nullptr) {
        msCentralPool_->RegisterLargeRegion(baseInfo);
        msCentralPool_->AddLargeRegionBytes(regionBytes);
    }
}

AllocBufferMS* RegionSpace::GetOrCreateAllocBufferMS()
{
    AllocBufferMS* buffer = ThreadLocal::GetAllocBufferMS();
    if (buffer == nullptr) {
        buffer = new (std::nothrow) AllocBufferMS();
        CHECK_DETAIL(buffer != nullptr, "new MS alloc buffer fail");
        buffer->Init(msCentralPool_);
        ThreadLocal::SetAllocBufferMS(buffer);
    }
    return buffer;
}
#endif

AllocBuffer* AllocBuffer::GetOrCreateAllocBuffer()
{
    auto* buffer = AllocBuffer::GetAllocBuffer();
    if (buffer == nullptr) {
        buffer = new (std::nothrow) AllocBuffer();
        CHECK_DETAIL(buffer != nullptr, "new region alloc buffer fail");
        buffer->Init();
        ThreadLocal::SetAllocBuffer(buffer);
    }
    return buffer;
}

AllocBuffer* AllocBuffer::GetAllocBuffer() { return ThreadLocal::GetAllocBuffer(); }

AllocBuffer::~AllocBuffer()
{
    if (LIKELY(tlRegion != RegionInfo::NullRegion()) && tlRegion != nullptr) {
        RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
        RegionManager& manager = theAllocator.GetRegionManager();
        manager.RemoveThreadLocalRegion(tlRegion);
        manager.EnlistFullThreadLocalRegion(tlRegion);
        tlRegion = RegionInfo::NullRegion();
    }
}

void AllocBuffer::Init()
{
    static_assert(offsetof(AllocBuffer, tlRegion) == 0,
                  "need to modify the offset of this value in llvm-project at the same time");
    tlRegion = RegionInfo::NullRegion();
    Heap::GetHeap().RegisterAllocBuffer(*this);
}

void AllocBuffer::Fini()
{
    Heap::GetHeap().RemoveAllocBuffer(*this);
}

MAddress AllocBuffer::Allocate(size_t totalSize, AllocType allocType)
{
    // a hoisted specific fast path which can be inlined
    MAddress addr = 0;
    if (UNLIKELY(allocType == AllocType::RAW_POINTER_OBJECT)) {
        return AllocateRawPointerObject(totalSize);
    }

    if (LIKELY(tlRegion != RegionInfo::NullRegion())) {
        addr = tlRegion->Alloc(totalSize);
    }

    if (UNLIKELY(addr == 0)) {
        addr = AllocateImpl(totalSize, allocType);
    }

    DLOG(ALLOC, "alloc 0x%zx(%zu)", addr, totalSize);
    return addr;
}

// try an allocation but do not handle failure
MAddress AllocBuffer::AllocateImpl(size_t totalSize, AllocType allocType)
{
    RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& manager = theAllocator.GetRegionManager();

    // allocate from thread local region
    if (LIKELY(tlRegion != RegionInfo::NullRegion())) {
        MAddress addr = tlRegion->Alloc(totalSize);
        if (addr != 0) {
            return addr;
        }

        // allocation failed because region is full.
        CHECK(tlRegion->IsThreadLocalRegion());
        {
            manager.RemoveThreadLocalRegion(tlRegion);
            manager.EnlistFullThreadLocalRegion(tlRegion);
            tlRegion = RegionInfo::NullRegion();
        }
    }

    // now region must be null. If a region has been ready, then use it and tell gc-assitant thread to prepare
    // a new region, or take a new one.
    RegionInfo* r  = preparedRegion.load(std::memory_order_acquire);
    if (r != nullptr) {
        preparedRegion.store(nullptr, std::memory_order_release);
        tlRegion = r;
        if (theAllocator.IsAsyncAllocationEnable()) {
            theAllocator.AddHungryBuffer(*this);
            Heap::GetHeap().GetFinalizerProcessor().NotifyToFeedAllocBuffers();
        }
        return r->Alloc(totalSize);
    }
    // AllocateThreadLocalRegion is a safepoint, in which cj thread rescheule may happen.
    // tlRegion is bound to specific thread, so we need to forbid reschedule.
    CJThreadPreemptOffCntAdd();
    r = manager.AllocateThreadLocalRegion();
    CJThreadPreemptOffCntSub();
    if (UNLIKELY(r == nullptr)) {
        return 0;
    }
    // tlRegion may be set in PreforwardPhase handler while allocating region.
    // Null region means tlRegion is not set.
    if (tlRegion == RegionInfo::NullRegion()) {
        tlRegion = r;
        return r->Alloc(totalSize);
    }
    // tlRegion has been set in preforward phase.
    MAddress addr = tlRegion->Alloc(totalSize);
    if (addr != 0) {
        if (!SetPreparedRegion(r)) {
            // r is inserted in thread-local region list when allocated, we need to remove it from the list.
            manager.RemoveThreadLocalRegion(r);
            manager.ReclaimRegion(r);
        }
        return addr;
    }
    // tlRegion is not enough for allocation, so we use r.
    manager.RemoveThreadLocalRegion(tlRegion);
    manager.EnlistFullThreadLocalRegion(tlRegion);
    tlRegion = r;
    return r->Alloc(totalSize);
}

MAddress AllocBuffer::AllocateRawPointerObject(size_t totalSize)
{
    RegionInfo* region = tlRawPointerRegions.GetHeadRegion();
    if (region != nullptr) {
        MAddress allocAddr = region->Alloc(totalSize);
        if (allocAddr != 0) {
            return allocAddr;
        }
    }
    RegionManager& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    size_t needUnitNum = AlignUp(totalSize, RegionInfo::UNIT_SIZE) / RegionInfo::UNIT_SIZE;
    if (totalSize <= manager.GetThreadLocalRegionSize()) {
        region = manager.TakeRegion(needUnitNum, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        if (region == nullptr) {
            return 0;
        }
        tlRawPointerRegions.PrependRegion(region, RegionInfo::RegionType::TL_RAW_POINTER_REGION);
    } else {
        region = manager.TakeRegion(needUnitNum, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
        if (region == nullptr) {
            return 0;
        }
        tlLargeRawPointerRegions.PrependRegion(region, RegionInfo::RegionType::TL_LARGE_RAW_POINTER_REGION);
    }

    // region is enough for totalSize.
    MAddress allocAddr = region->Alloc(totalSize);
    MRT_ASSERT(allocAddr != 0, "allocation failure");
    return allocAddr;
}

void AllocBuffer::CommitRawPointerRegions()
{
    RegionManager& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    manager.MergeRawPointerRegions(tlRawPointerRegions, tlLargeRawPointerRegions);
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
bool RegionSpace::IsHeapObject(MAddress addr) const
{
    return IsHeapAddress(addr);
}
#endif
void RegionSpace::FeedHungryBuffers()
{
#ifdef MRT_USE_MARK_SWEEP
    // MS mode uses AllocBufferMS + CentralRegionPool, not AllocBuffer + RegionManager.
    return;
#else
    ScopedObjectAccess soa;
    AllocBufferManager::HungryBuffers hungryBuffers;
    allocBufferManager->SwapHungryBuffers(hungryBuffers);
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
#endif
}
} // namespace MapleRuntime
