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

// Helper: check if the mutator is in a GC marking phase
static bool IsInMarkingPhase()
{
    GCPhase phase = Mutator::GetMutator()->GetMutatorPhase();
    return phase == GCPhase::GC_PHASE_MS_MARK_SETUP ||
           phase == GCPhase::GC_PHASE_MS_CONCURRENT_MARK ||
           phase == GCPhase::GC_PHASE_MS_MARK_TERMINATION;
}

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
    allocCount_++;
    if (UNLIKELY(cls >= kNumSizeClasses)) {
        return 0;  // Large object — caller should route to LargeObjectSpace
    }
    MAddress addr = FastAlloc(cls);

    // Mark-at-alloc: during GC marking phases, newly allocated objects are
    // colored black (marked) so they survive the current GC cycle.
    // This is equivalent to Go's gcmarknewobject() in mallocgc().
    if (UNLIKELY(addr != 0 && IsInMarkingPhase())) {
        RegionSpace::MarkObject(reinterpret_cast<const BaseObject*>(addr));
    }

    // Mutator assist: accumulate debt and trigger marking assist.
    // Only during GC_PHASE_MS_CONCURRENT_MARK — not setup (marking not started)
    // or termination (STW, no Mutator allocation).
    if (UNLIKELY(addr != 0)) {
        GCPhase phase = Mutator::GetMutator()->GetMutatorPhase();
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

    return addr;
}

// ============================================================================
// FastAlloc — try allocation on the current active Region
// ============================================================================
MAddress AllocBufferMS::FastAlloc(uint8_t cls)
{
    MSRegionInfo* region = activeRegions_[cls];
    if (LIKELY(region != nullptr)) {
        uintptr_t addr;
        if (region->regionType == MSRegionType::MS_REGION_BUMP) {
            // Fresh (BUMP) Region: use bump-pointer allocation.
            // Alloc() advances allocPtr and syncs the allocBitmap bookkeeping.
            addr = region->Alloc();
        } else {
            // Swept Region: use bitmap-based allocation (CTZ scan).
            addr = region->AllocFromBitmap();
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

    // Step 3: Set as active Region
    activeRegions_[cls] = newRegion;

    // Step 4: Allocate from the new Region (respect region type)
    uintptr_t addr;
    if (newRegion->regionType == MSRegionType::MS_REGION_BUMP) {
        addr = newRegion->Alloc();
    } else {
        addr = newRegion->AllocFromBitmap();
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
