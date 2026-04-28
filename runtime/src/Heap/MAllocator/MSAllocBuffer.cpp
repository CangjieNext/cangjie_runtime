// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "MSRegionSpace.h"
#include "Heap.h"
#include "Mutator/Mutator.h"
#include "Base/Log.h"
#include "Collector/CollectorResources.h"
#include "Heap/MCollector/MarkSweepCollector.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace MapleRuntime {

static constexpr bool kDisableMutatorAssistProbe = false;
static constexpr bool kDisableSweepOnRegionMissProbe = false;

static inline bool IsMSMarkingActive(uint8_t gcFastState)
{
    return (gcFastState & ThreadLocal::GC_FAST_STATE_MS_MARKING) != 0;
}

static inline bool IsMSAssistActive(uint8_t gcFastState)
{
    return (gcFastState & ThreadLocal::GC_FAST_STATE_MS_ASSIST) != 0;
}

static inline uint8_t ResolveGCFastStateForAllocFastPath(ThreadLocalData* local)
{
    return local != nullptr ? local->gcFastState : ThreadLocal::GetGCFastState();
}

static inline uintptr_t FastAllocBump(uintptr_t& allocPtr, uintptr_t regionEnd, size_t slotSize)
{
    uintptr_t next = allocPtr + slotSize;
    if (LIKELY(next <= regionEnd)) {
        uintptr_t addr = allocPtr;
        allocPtr = next;
        return addr;
    }
    return 0;
}

static inline uintptr_t FastAllocBitmap(MSRegionInfo* region, size_t slotSize)
{
    uint64_t summary = region->reserved;
    while (summary != 0) {
        size_t wordIdx = static_cast<size_t>(__builtin_ctzll(summary));
        uint64_t word = region->allocBitmap[wordIdx];
        if (LIKELY(word != 0)) {
            int bit = __builtin_ctzll(word);
            region->allocBitmap[wordIdx] = word & ~(1ULL << bit);
            region->freeSlotCount--;
            if (region->allocBitmap[wordIdx] == 0) {
                region->reserved &= ~(1ULL << wordIdx);
            }
            size_t slotIndex = wordIdx * 64 + static_cast<size_t>(bit);
            return region->RegionStart() + slotIndex * slotSize;
        }
        summary &= ~(1ULL << wordIdx);
        region->reserved &= ~(1ULL << wordIdx);
    }
    return 0;
}

AllocBufferMS::~AllocBufferMS()
{
    Fini();
}

AllocBufferMS::AllocBufferMS()
{
    // Ensure tlRegion_ is at offset 0 — same as AllocBuffer::tlRegion.
    // LLVM fast-path reads *(buffer + 0) as RegionInfo* and calls Alloc().
    static_assert(offsetof(AllocBufferMS, tlRegion_) == 0,
                  "tlRegion_ must be at offset 0 for LLVM fast-path compatibility");
    tlRegion_ = RegionInfo::NullRegion();
}

void AllocBufferMS::Init(CentralRegionPool* central)
{
    central_ = central;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        SetActiveRegion(cls, nullptr);
    }
    // Register with CentralRegionPool so GC can FlushAll
    if (central_ != nullptr) {
        central_->RegisterAllocBufferMS(this);
    }
}

void AllocBufferMS::Fini()
{
    FlushAll();
    // Unregister from CentralRegionPool before clearing
    if (central_ != nullptr) {
        central_->RemoveAllocBufferMS(this);
    }
    central_ = nullptr;
}

void AllocBufferMS::SetActiveRegion(uint8_t cls, MSRegionInfo* region)
{
    activeRegions_[cls] = region;
    if (region == nullptr) {
        activeAllocKinds_[cls] = ActiveAllocKind::NONE;
        activeAllocPtrs_[cls] = 0;
        activeRegionEnds_[cls] = 0;
        return;
    }

    activeAllocKinds_[cls] = region->regionType == MSRegionType::MS_REGION_BUMP ?
        ActiveAllocKind::BUMP : ActiveAllocKind::BITMAP;
    if (activeAllocKinds_[cls] == ActiveAllocKind::BUMP) {
        activeAllocPtrs_[cls] = region->allocPtr;
        activeRegionEnds_[cls] = region->regionEnd;
    } else {
        activeAllocPtrs_[cls] = 0;
        activeRegionEnds_[cls] = 0;
    }
}

void AllocBufferMS::SyncActiveBumpRegionShadow(uint8_t cls)
{
    if (activeAllocKinds_[cls] != ActiveAllocKind::BUMP) {
        return;
    }

    MSRegionInfo* region = activeRegions_[cls];
    if (region == nullptr) {
        return;
    }

    uintptr_t allocPtr = activeAllocPtrs_[cls];
    uintptr_t regionEnd = activeRegionEnds_[cls];
    region->allocPtr = allocPtr;
    region->regionEnd = regionEnd;
    size_t slotSize = SizeClassInfo::ClassToSize(cls);
    region->freeSlotCount = static_cast<uint16_t>((regionEnd - allocPtr) / slotSize);
}

// ============================================================================
// Allocate — main entry point
// ============================================================================
MAddress AllocBufferMS::Allocate(size_t size, ThreadLocalData* local)
{
    uint8_t cls = SizeClassInfo::SizeToClass(size);
    if (UNLIKELY(cls >= kNumSizeClasses)) {
        return 0;  // Large object — caller handles it via the large-region path
    }
    MAddress addr = FastAlloc(cls);

    if (UNLIKELY(addr == 0)) {
        return addr;
    }

    uint8_t gcFastState = ResolveGCFastStateForAllocFastPath(local);
    if (LIKELY(!IsMSMarkingActive(gcFastState))) {
        gcAssistDebtScaled_ = 0;
        return addr;
    }

    // Fresh objects allocated during MS marking are always heap objects here,
    // so we can mark them directly without going through collector lookup.
    (void)MSRegionInfoArray::MarkObjectInHeap(static_cast<uintptr_t>(addr));

    if (kDisableMutatorAssistProbe) {
        gcAssistDebtScaled_ = 0;
        return addr;
    }

    if (UNLIKELY(!IsMSAssistActive(gcFastState))) {
        gcAssistDebtScaled_ = 0;
        return addr;
    }

    MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
    if (UNLIKELY(collector == nullptr)) {
        gcAssistDebtScaled_ = 0;
        return addr;
    }

    // Go-style dynamic pacing: debt += allocatedBytes * assistRatio.
    uint64_t ratioScaled = collector->GetAssistWorkPerByteScaled();
    if (ratioScaled > 0) {
        uint64_t deltaDebt = static_cast<uint64_t>(size) * ratioScaled;
        if (UNLIKELY(gcAssistDebtScaled_ > UINT64_MAX - deltaDebt)) {
            gcAssistDebtScaled_ = UINT64_MAX;
        } else {
            gcAssistDebtScaled_ += deltaDebt;
        }

        const uint64_t ratioScale = MarkSweepCollector::GetAssistRatioScale();
        size_t debtObjects = static_cast<size_t>(gcAssistDebtScaled_ / ratioScale);
        if (UNLIKELY(debtObjects >= collector->GetAssistDebtTriggerObjects())) {
            // Call MSMarkAssist unconditionally: it first tries bgScanCredit
            // (even when the mark work queue is empty), then does real marking
            // only if credit is insufficient and grey objects are available.
            // The old hasWork guard was wrong: it prevented credit consumption
            // when the work queue happened to be empty, causing debt to pile up
            // and trigger burst assist later.
            size_t paidObjects = collector->MSMarkAssist(debtObjects);
            size_t accounted = std::min(paidObjects, debtObjects);
            gcAssistDebtScaled_ -= static_cast<uint64_t>(accounted) * ratioScale;
        }
    }

    return addr;
}

// ============================================================================
// FastAlloc — try allocation on the current active Region
// ============================================================================
MAddress AllocBufferMS::FastAlloc(uint8_t cls)
{
    MSRegionInfo* region = activeRegions_[cls];
    if (LIKELY(region != nullptr)) {
#ifdef MRT_DEBUG
        if (UNLIKELY(region->regionType == MSRegionType::MS_REGION_LARGE ||
                     region->regionType == MSRegionType::MS_REGION_FREE ||
                     region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE)) {
            LOG(RTLOG_FATAL, "[MS-GC] FastAlloc: activeRegion has bad type=%d, cls=%u, "
                "region=%p, allocPtr=0x%zx, regionEnd=0x%zx",
                (int)region->regionType, (unsigned)cls, (void*)region,
                region->allocPtr, region->regionEnd);
        }
#endif

        uintptr_t addr;
        switch (activeAllocKinds_[cls]) {
            case ActiveAllocKind::BUMP:
                addr = FastAllocBump(activeAllocPtrs_[cls], activeRegionEnds_[cls], SizeClassInfo::ClassToSize(cls));
                break;
            case ActiveAllocKind::BITMAP:
                addr = FastAllocBitmap(region, SizeClassInfo::ClassToSize(cls));
                break;
            case ActiveAllocKind::NONE:
            default:
                addr = 0;
                break;
        }
        if (LIKELY(addr != 0)) {
            return static_cast<MAddress>(addr);
        }
    }

    // Current Region has no free slots (or no Region at all) — slow path
    return SlowAlloc(cls);
}

// ============================================================================
// SlowAlloc — refill from CentralRegionPool
// ============================================================================
MAddress AllocBufferMS::SlowAlloc(uint8_t cls)
{
    // Step 1: Return the exhausted Region to the central pool
    if (activeRegions_[cls] != nullptr) {
        ReturnRegion(cls);
    }

    // O2: Check thread-local cache before going to CentralRegionPool
    MSRegionInfo* newRegion = TakeCachedRegion(cls);
    if (newRegion == nullptr) {
        if (!kDisableSweepOnRegionMissProbe && central_ != nullptr) {
            central_->MaybeNotifySweep(cls);
        }
        if (cacheMiss_[cls] < UINT8_MAX) {
            ++cacheMiss_[cls];
        }
        if (cacheMiss_[cls] >= kCacheFillThreshold) {
            Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor().NotifyToFeedAllocBuffers();
        }
        // Step 2: Get a new Region from CentralRegionPool
        newRegion = RefillFromCentral(cls);
    }
    if (UNLIKELY(newRegion == nullptr)) {
        return 0;  // OOM or GC needed — caller handles
    }

    // Debug: validate newly-acquired region
#ifdef MRT_DEBUG
    {
        size_t span = newRegion->regionEnd - newRegion->allocPtr;
        size_t ridx = MSRegionInfoArray::GetRegionIdx(newRegion);
        uintptr_t expectedEnd = MSRegionInfoArray::GetRegionAddress(ridx) + MS_REGION_SIZE;
        if (UNLIKELY(newRegion->regionEnd != expectedEnd)) {
            LOG(RTLOG_FATAL, "[MS-GC] SlowAlloc: region has wrong regionEnd! "
                "regionEnd=0x%zx, expected=0x%zx, allocPtr=0x%zx, "
                "cls=%u, regionType=%d, sweepState=%d, sizeClass=%u, "
                "allocBitmap=%p, markBitmap=%p, "
                "totalSlots=%u, freeSlots=%u, "
                "nextRegionIdx=%d, prevRegionIdx=%d, "
                "sweepGen=%u, reserved=0x%llx, "
                "this=%p (idx=%zu)",
                newRegion->regionEnd, expectedEnd, newRegion->allocPtr,
                (unsigned)cls, (int)newRegion->regionType, (int)newRegion->sweepState,
                (unsigned)newRegion->sizeClass,
                (void*)newRegion->allocBitmap, (void*)newRegion->markBitmap,
                (unsigned)newRegion->totalSlotCount, (unsigned)newRegion->freeSlotCount,
                newRegion->nextRegionIdx, newRegion->prevRegionIdx,
                newRegion->sweepGen, (unsigned long long)newRegion->reserved,
                (void*)newRegion, ridx);
        }
        if (UNLIKELY(newRegion->allocBitmap == nullptr)) {
            LOG(RTLOG_FATAL, "[MS-GC] SlowAlloc: region has allocBitmap=nullptr, "
                "cls=%u, regionType=%d, allocPtr=0x%zx, regionEnd=0x%zx, "
                "this=%p (idx=%zu)",
                (unsigned)cls, (int)newRegion->regionType,
                newRegion->allocPtr, newRegion->regionEnd,
                (void*)newRegion, ridx);
        }
    }
#endif

    // Step 3: Set as active Region
    SetActiveRegion(cls, newRegion);

    // Backup cache refill is handled by BackgroundFillCache after repeated misses.
    // Avoid doing another synchronous GetRegion() here: on case1 this can turn one
    // SlowAlloc miss into two central-pool refills, and the second refill may also
    // fall into lazy sweep on the mutator thread.

    // Step 4: Allocate from the new Region (respect region type)
    uintptr_t addr;
    if (activeAllocKinds_[cls] == ActiveAllocKind::BUMP) {
        addr = FastAllocBump(activeAllocPtrs_[cls], activeRegionEnds_[cls], SizeClassInfo::ClassToSize(cls));
    } else {
        addr = FastAllocBitmap(newRegion, SizeClassInfo::ClassToSize(cls));
    }
    return static_cast<MAddress>(addr);
}

// ============================================================================
// ReturnRegion — classify and return active Region to CentralRegionPool
// ============================================================================
void AllocBufferMS::ReturnRegion(uint8_t cls)
{
    MSRegionInfo* region = activeRegions_[cls];
    if (region == nullptr) {
        return;
    }

    SyncActiveBumpRegionShadow(cls);

    ReturnRegionToCentral(region, cls);
    SetActiveRegion(cls, nullptr);
}

MSRegionInfo* AllocBufferMS::TakeCachedRegion(uint8_t cls)
{
    for (uint8_t slot = 0; slot < kCachedRegionSlots; ++slot) {
        MSRegionInfo* region = cachedRegion_[cls][slot].exchange(nullptr, std::memory_order_acquire);
        if (region != nullptr) {
            return region;
        }
    }
    return nullptr;
}

bool AllocBufferMS::TryCacheRegion(uint8_t cls, MSRegionInfo* region)
{
    for (uint8_t slot = 0; slot < kCachedRegionSlots; ++slot) {
        MSRegionInfo* expected = nullptr;
        if (cachedRegion_[cls][slot].compare_exchange_strong(expected, region,
                std::memory_order_release, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void AllocBufferMS::ReturnRegionToCentral(MSRegionInfo* region, uint8_t cls)
{
    if (region == nullptr) {
        return;
    }

    // Experimental policy: TL regions returned after the cycle boundary are
    // stamped as swept in the current generation so they participate in the
    // next GC cycle instead of the current sweep.
    region->SetSweepGen(CentralRegionPool::GetGlobalSweepGen());

    bool isFree = region->IsCompletelyFree();
    bool hasSpace = region->HasFreeSpace();

    if (isFree) {
        central_->ReturnEmptyRegion(region);
    } else if (hasSpace) {
        // Has free slots → partial list
        central_->ReturnPartialRegion(region, cls);
    } else {
        // Full → full list
        central_->ReturnFullRegion(region, cls);
    }
}

// ============================================================================
// RefillFromCentral — get a Region from CentralRegionPool
// ============================================================================
MSRegionInfo* AllocBufferMS::RefillFromCentral(uint8_t cls)
{
    if (UNLIKELY(central_ == nullptr)) {
        return nullptr;
    }

    // Priority 1: partial Region with free slots (from CentralRegionPool's 4-list)
    MSRegionInfo* region = central_->GetRegion(cls);
    if (region != nullptr) {
        return region;
    }

    // Priority 2: batch-fetch fresh regions so one slow-path refill can populate
    // the active region plus a couple of thread-local backups.
    constexpr uint8_t kFreshBatchCount = 1 + kCachedRegionSlots;
    MSRegionInfo* freshRegions[kFreshBatchCount] = {};
    uint8_t freshCount = central_->GetFreshRegionBatch(cls, freshRegions, kFreshBatchCount);
    if (freshCount == 0) {
        return nullptr;
    }

    for (uint8_t index = 1; index < freshCount; ++index) {
        if (!TryCacheRegion(cls, freshRegions[index])) {
            central_->ReturnPartialRegion(freshRegions[index], cls);
        }
    }
    return freshRegions[0];
}

// ============================================================================
// FlushAll — GC STW: return all active Regions to CentralRegionPool
// ============================================================================
void AllocBufferMS::FlushAll()
{
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        // O2: Return cached backup regions first
        for (uint8_t slot = 0; slot < kCachedRegionSlots; ++slot) {
            MSRegionInfo* cached = cachedRegion_[cls][slot].exchange(nullptr, std::memory_order_acquire);
            if (cached != nullptr) {
                // markBitmap clearing is now handled by ClearAllMarkBitmaps
                // before STW, so no per-region ClearMarkBitmap here.
                ReturnRegionToCentral(cached, cls);
            }
        }

        // Active region: return to pool (bitmap already cleared pre-STW)
        ReturnRegion(cls);
    }
}

// ============================================================================
// BackgroundFillCache — O9: adaptive prefetch by gc-helper thread
// ============================================================================
void AllocBufferMS::BackgroundFillCache()
{
    if (UNLIKELY(central_ == nullptr)) {
        return;
    }
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        if (cacheMiss_[cls] < kCacheFillThreshold) {
            continue;
        }
        bool cacheFull = true;
        for (uint8_t slot = 0; slot < kCachedRegionSlots; ++slot) {
            if (cachedRegion_[cls][slot].load(std::memory_order_relaxed) != nullptr) {
                continue;
            }
            cacheFull = false;
            MSRegionInfo* region = central_->GetRegion(cls);
            if (region == nullptr) {
                if (!kDisableSweepOnRegionMissProbe) {
                    central_->MaybeNotifySweep(cls);
                }
                break;
            }
            if (!TryCacheRegion(cls, region)) {
                central_->ReturnPartialRegion(region, cls);
                break;
            }
        }
        if (cacheFull) {
            cacheMiss_[cls] = 0;
            continue;
        }

        bool allSlotsFilled = true;
        for (uint8_t slot = 0; slot < kCachedRegionSlots; ++slot) {
            if (cachedRegion_[cls][slot].load(std::memory_order_relaxed) == nullptr) {
                allSlotsFilled = false;
                break;
            }
        }
        if (allSlotsFilled) {
            cacheMiss_[cls] = 0;
        }
    }
}

AllocBufferMS* AllocBufferMS::GetAllocBufferMS()
{
    return ThreadLocal::GetAllocBufferMS();
}

AllocBufferMS* AllocBufferMS::GetOrCreateAllocBufferMS()
{
    return GetOrCreateAllocBufferMS(ThreadLocal::GetThreadLocalData());
}

AllocBufferMS* AllocBufferMS::GetOrCreateAllocBufferMS(ThreadLocalData* local)
{
    RegionSpaceMS& space = reinterpret_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    return GetOrCreateAllocBufferMS(local, space.GetCentralPool());
}

AllocBufferMS* AllocBufferMS::GetOrCreateAllocBufferMS(ThreadLocalData* local, CentralRegionPool* central)
{
    AllocBufferMS* buffer = local != nullptr ? local->msAllocBuffer : GetAllocBufferMS();
    if (buffer == nullptr) {
        buffer = new (std::nothrow) AllocBufferMS();
        CHECK_DETAIL(buffer != nullptr, "new MS alloc buffer fail");
        buffer->Init(central);
        if (local != nullptr) {
            local->msAllocBuffer = buffer;
        } else {
            ThreadLocal::SetAllocBufferMS(buffer);
        }
    }
    return buffer;
}

// ============================================================================
// AccumulatePacingDebt — pacing debt accumulation for paths bypassing Allocate()
// (e.g. large-object allocation in TryAllocateOnceMS)
// ============================================================================
void AllocBufferMS::AccumulatePacingDebt(size_t size)
{
    if (kDisableMutatorAssistProbe) {
        gcAssistDebtScaled_ = 0;
        return;
    }

    MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
    if (collector == nullptr) {
        return;
    }
    uint64_t ratioScaled = collector->GetAssistWorkPerByteScaled();
    if (ratioScaled == 0) {
        return;
    }
    uint64_t deltaDebt = static_cast<uint64_t>(size) * ratioScaled;
    if (UNLIKELY(gcAssistDebtScaled_ > UINT64_MAX - deltaDebt)) {
        gcAssistDebtScaled_ = UINT64_MAX;
    } else {
        gcAssistDebtScaled_ += deltaDebt;
    }
    const uint64_t ratioScale = MarkSweepCollector::GetAssistRatioScale();
    size_t debtObjects = static_cast<size_t>(gcAssistDebtScaled_ / ratioScale);
    if (UNLIKELY(debtObjects >= collector->GetAssistDebtTriggerObjects())) {
        size_t paidObjects = collector->MSMarkAssist(debtObjects);
        size_t accounted = std::min(paidObjects, debtObjects);
        gcAssistDebtScaled_ -= static_cast<uint64_t>(accounted) * ratioScale;
    }
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
