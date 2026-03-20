// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "AllocBufferMS.h"
#include "CentralRegionPool.h"
#include "RegionSpace.h"
#include "Mutator/Mutator.h"
#include "Base/Log.h"
#include "Heap/Collector/MarkSweepCollector.h"
#include <cstdio>

namespace MapleRuntime {

AllocBufferMS::~AllocBufferMS()
{
    Fini();
}

void AllocBufferMS::Init(CentralRegionPool* central)
{
    central_ = central;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        activeRegions_[cls] = nullptr;
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

// ============================================================================
// Allocate — main entry point
// ============================================================================
MAddress AllocBufferMS::Allocate(size_t size)
{
    uint8_t cls = SizeClassInfo::SizeToClass(size);
    if (UNLIKELY(cls >= kNumSizeClasses)) {
        return 0;  // Large object — caller should route to LargeObjectSpace
    }
    MAddress addr = FastAlloc(cls);

    // Mark-at-alloc: during ALL MS-GC phases (MARK_SETUP through SWEEP),
    // newly allocated objects must be colored black (marked) so they survive
    // the current GC cycle's sweep.
    //
    // Critical: objects allocated during MS_SWEEP phase are placed in free
    // slots of already-swept regions (using old allocBitmap). Their markBitmap
    // bits are 0 (cleared at MarkSetup). When PrepareForSweep flips the
    // generation and the region is lazily swept again, allocBitmap = ~markBitmap
    // would incorrectly free these live objects. Setting the mark bit here
    // prevents that.
    if (UNLIKELY(addr != 0)) {
        GCPhase phase = Mutator::GetMutator()->GetMutatorPhase();
        if (UNLIKELY(phase >= GCPhase::GC_PHASE_MS_MARK_SETUP &&
                     phase <= GCPhase::GC_PHASE_MS_SWEEP)) {
            RegionSpace::MarkObject(reinterpret_cast<const BaseObject*>(addr));
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
                     region->regionType == MSRegionType::MS_REGION_FREE)) {
            LOG(RTLOG_FATAL, "[MS-GC] FastAlloc: activeRegion has bad type=%d, cls=%u, "
                "region=%p, allocPtr=0x%zx, regionEnd=0x%zx",
                (int)region->regionType, (unsigned)cls, (void*)region,
                region->allocPtr, region->regionEnd);
        }
#endif

        uintptr_t addr;
        if (region->regionType == MSRegionType::MS_REGION_BUMP) {
            addr = region->AllocBump();
        } else {
            addr = region->AllocFromBitmapFast();
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
    MSRegionInfo* newRegion = cachedRegion_[cls];
    if (newRegion != nullptr) {
        cachedRegion_[cls] = nullptr;
    } else {
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
    activeRegions_[cls] = newRegion;

    // O2: Prefetch one backup region into cache for next SlowAlloc
    if (cachedRegion_[cls] == nullptr && central_ != nullptr) {
        cachedRegion_[cls] = central_->GetRegion(cls);
    }

    // Step 4: Allocate from the new Region (respect region type)
    uintptr_t addr;
    if (newRegion->regionType == MSRegionType::MS_REGION_BUMP) {
        addr = newRegion->AllocBump();
    } else {
        addr = newRegion->AllocFromBitmapFast();
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

    bool isFree = region->IsCompletelyFree();
    bool hasSpace = region->HasFreeSpace();

    if (isFree) {
        // O3: Reset to BUMP for fast reuse, keep in CentralRegionPool
        region->ResetToBump();
        central_->ReturnPartialRegion(region, cls);
    } else if (hasSpace) {
        // Has free slots → partial list
        central_->ReturnPartialRegion(region, cls);
    } else {
        // Full → full list
        central_->ReturnFullRegion(region, cls);
    }

    activeRegions_[cls] = nullptr;
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

    // Priority 2: brand-new Region from RegionManager
    region = central_->GetFreshRegion(cls);
    return region;
}

// ============================================================================
// FlushAll — GC STW: return all active Regions to CentralRegionPool
// ============================================================================
void AllocBufferMS::FlushAll()
{
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        // O2: Return cached backup regions first
        if (cachedRegion_[cls] != nullptr) {
            MSRegionInfo* cached = cachedRegion_[cls];
            cachedRegion_[cls] = nullptr;
            if (cached->IsCompletelyFree()) {
                cached->ResetToBump();
                central_->ReturnPartialRegion(cached, cls);
            } else if (cached->HasFreeSpace()) {
                central_->ReturnPartialRegion(cached, cls);
            } else {
                central_->ReturnFullRegion(cached, cls);
            }
        }
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
        if (cachedRegion_[cls].load(std::memory_order_relaxed) != nullptr) {
            cacheMiss_[cls] = 0;
            continue;
        }
        MSRegionInfo* region = central_->GetRegion(cls);
        if (region == nullptr) {
            continue;
        }
        MSRegionInfo* expected = nullptr;
        if (cachedRegion_[cls].compare_exchange_strong(expected, region,
                std::memory_order_release, std::memory_order_relaxed)) {
            cacheMiss_[cls] = 0;
        } else {
            central_->ReturnPartialRegion(region, cls);
        }
    }
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
