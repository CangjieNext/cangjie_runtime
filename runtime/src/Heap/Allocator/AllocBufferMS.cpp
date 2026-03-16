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

    // Mark-at-alloc + mutator assist: single phase read covers both checks.
    // During GC marking phases, newly allocated objects are colored black
    // (marked) so they survive the current GC cycle.
    if (UNLIKELY(addr != 0)) {
        GCPhase phase = Mutator::GetMutator()->GetMutatorPhase();
        if (UNLIKELY(phase >= GCPhase::GC_PHASE_MS_MARK_SETUP &&
                     phase <= GCPhase::GC_PHASE_MS_MARK_TERMINATION)) {
            RegionSpace::MarkObject(reinterpret_cast<const BaseObject*>(addr));
            if (phase == GCPhase::GC_PHASE_MS_CONCURRENT_MARK) {
                gcAssistBytes_ += (size >> 2);  // ~25% of allocated size
                if (gcAssistBytes_ >= kGCAssistTriggerBytes) {
                    MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
                    if (collector != nullptr) {
                        collector->MSMarkAssist();
                    }
                    gcAssistBytes_ = 0;
                }
            }
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

    // Step 2: Get a new Region from CentralRegionPool
    MSRegionInfo* newRegion = RefillFromCentral(cls);
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
        // Entirely free → release back to FreeRegionManager
        central_->ReleaseRegion(region, cls);
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
        ReturnRegion(cls);
    }
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
