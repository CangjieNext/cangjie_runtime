// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Allocator/LOSManager.h"

#include "Allocator/ReserveManager.h"
#include "Base/LogFile.h"
#include "Collector/Collector.h"
#include "Heap.h"

namespace MapleRuntime {

void LOSManager::Initialize(uintptr_t start, uintptr_t end, UnitCount unitStart, UnitCount unitEnd)
{
    losStart = start;
    losEnd = end;
    losUnitStart = unitStart;
    losUnitEnd = unitEnd;

    // Initialize inactive zone at the end (LOS grows toward low address)
    losInactiveZone.store(losEnd, std::memory_order_relaxed);

    // Initialize FreeRegionManager for LOS space.
    // The FreeRegionManager's baseUnit was set to losUnitStart in constructor via
    // RegionManager::Initialize(). Here we just initialize the CartesianTrees
    // with the LOS unit count. The trees use local indices [0, losUnitCount),
    // and baseUnit offset is applied transparently when accessing RegionInfo.
    UnitCount losUnitCount = unitEnd - unitStart;
    losFreeMgr.Initialize(losUnitCount);

    DLOG(REPORT, "LOSManager initialized: space [0x%zx, 0x%zx), units [%u, %u), capacity %zu bytes, "
         "FreeRegionManager baseUnit=%u",
         losStart, losEnd, losUnitStart, losUnitEnd, GetCapacity(), losFreeMgr.GetBaseUnit());
}

// ===== M2: Large object allocation =====

uintptr_t LOSManager::AllocLarge(size_t size)
{
    size_t unitCount = (size + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;

    // Step 1: Try losFreeMgr (recycled regions from dirtyTree / releaseTree)
    RegionInfo* region = losFreeMgr.TakeRegion(unitCount, RegionInfo::UnitRole::LARGE_SIZED_UNITS, false);
    if (region != nullptr) {
        uintptr_t addr = region->Alloc(size);
        DLOG(REGION, "LOS alloc large from freeMgr: region @[0x%zx+%zu, 0x%zx) size %zu",
             region->GetRegionStart(), region->GetRegionSize(), region->GetRegionEnd(), size);
        EnlistLargeRegion(region);
        losAllocCount++;
        return addr;
    }

    // Step 2: Expand losInactiveZone toward low address
    region = AllocFromInactiveZone(unitCount);
    if (region != nullptr) {
        uintptr_t addr = region->Alloc(size);
        DLOG(REGION, "LOS alloc large from inactiveZone: region @[0x%zx+%zu, 0x%zx) size %zu",
             region->GetRegionStart(), region->GetRegionSize(), region->GetRegionEnd(), size);
        EnlistLargeRegion(region);
        losAllocCount++;
        return addr;
    }

    // Step 3: Borrow space from RESERVE zone (chunk strategy handled internally by ReserveManager)
    if (reserveManager != nullptr) {
        size_t regionSize = unitCount * RegionInfo::UNIT_SIZE;
        uintptr_t borrowedAddr = reserveManager->BorrowForLOS(regionSize);
        if (borrowedAddr != 0) {
            region = RegionInfo::InitRegionAt(borrowedAddr, unitCount,
                                              RegionInfo::UnitRole::LARGE_SIZED_UNITS);
            uintptr_t addr = region->Alloc(size);
            DLOG(REGION, "LOS alloc large from RESERVE: region @[0x%zx+%zu, 0x%zx) size %zu",
                 region->GetRegionStart(), region->GetRegionSize(), region->GetRegionEnd(), size);
            EnlistLargeRegion(region);
            losAllocCount++;
            losReserveBorrowCount++;
            return addr;
        }
    }

    DLOG(REGION, "LOS alloc large failed: size %zu (%zu units), "
         "freeMgr dirty=%u released=%u, inactiveZone=0x%zx, losStart=0x%zx",
         size, unitCount,
         losFreeMgr.GetDirtyUnitCount(), losFreeMgr.GetReleasedUnitCount(),
         losInactiveZone.load(std::memory_order_relaxed), losStart);

    losAllocFailCount++;
    return 0;
}

RegionInfo* LOSManager::AllocFromInactiveZone(size_t unitCount)
{
    size_t regionSize = unitCount * RegionInfo::UNIT_SIZE;
    uintptr_t currentZone = losInactiveZone.load(std::memory_order_relaxed);

    // LOS grows from losEnd toward losStart (high → low address)
    if (currentZone < losStart + regionSize) {
        return nullptr;  // not enough space
    }

    // Try CAS to claim the inactive zone
    uintptr_t newZone = currentZone - regionSize;
    while (!losInactiveZone.compare_exchange_weak(currentZone, newZone,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
        if (currentZone < losStart + regionSize) {
            return nullptr;
        }
        newZone = currentZone - regionSize;
    }

    // newZone is the start address of the new region
    RegionInfo* region = RegionInfo::InitRegionAt(newZone, unitCount, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
    DLOG(REGION, "LOS inactive zone expanded: [0x%zx, 0x%zx) -> 0x%zx, allocated %zu units",
         newZone, newZone + regionSize, newZone, unitCount);
    return region;
}

void LOSManager::EnlistLargeRegion(RegionInfo* region)
{
    // Same logic as RegionManager::AllocLarge for region list management:
    // During GC trace phase, put into trace region cache; otherwise into recent list.
    GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase();
    bool shouldSetTraceRegion = (phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER);
    if (losLargeTraceRegions.TryPrependRegion(region, RegionInfo::RegionType::RECENT_LARGE_REGION)) {
        if (shouldSetTraceRegion) {
            region->SetTraceRegionFlag(1);
        }
    } else {
        losRecentLargeRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_LARGE_REGION);
        region->SetTraceRegionFlag(0);
    }
}

} // namespace MapleRuntime
