// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Allocator/LOSManager.h"

#include "Allocator/RegionManager.h"
#include "Allocator/RegionSpace.h"
#include "Allocator/ReserveManager.h"
#include "Base/LogFile.h"
#include "Collector/Collector.h"
#include "Heap.h"
#include "ObjectModel/RefField.inline.h"

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

// ===== M3: LOS GC Sweep =====

void LOSManager::AssembleLOSGarbageCandidates()
{
    // Merge recent → old, then clear live info for marking
    losOldLargeRegionList.MergeRegionList(losRecentLargeRegionList, RegionInfo::RegionType::LARGE_REGION);
    for (RegionInfo* region = losOldLargeRegionList.GetHeadRegion();
         region != nullptr; region = region->GetNextRegion()) {
        region->ClearLiveInfo();
    }
    DLOG(REGION, "LOS AssembleGarbageCandidates: old list assembled");
}

void LOSManager::HandleLOSTraceRegions()
{
    losLargeTraceRegions.DeactivateRegionCache();
    losRecentLargeRegionList.MergeRegionList(losLargeTraceRegions, RegionInfo::RegionType::RECENT_LARGE_REGION);
}

size_t LOSManager::CollectLOSGarbage()
{
    size_t garbageSize = 0;
    losGarbageCollectedBytes = 0;

    RegionInfo* region = losOldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        // Large region has one object at offset 0
        if (!region->IsSurvivedObject(0)) {
            // Dead — reclaim to LOS's losFreeMgr (NOT MAIN's freeRegionManager)
            DLOG(REGION, "LOS reclaim dead region %p@[0x%zx+%zu, 0x%zx) type %u",
                 region, region->GetRegionStart(),
                 region->GetRegionAllocatedSize(), region->GetRegionEnd(),
                 region->GetRegionType());

            RegionInfo* del = region;
            region = region->GetNextRegion();
            losOldLargeRegionList.DeleteRegion(del);

            if (del->GetRegionSize() > RegionInfo::LARGE_OBJECT_RELEASE_THRESHOLD) {
                garbageSize += ReleaseLOSRegion(del);
            } else {
                garbageSize += ReclaimLOSRegion(del);
            }
        } else {
            // Survived — reset mark bit for next GC cycle
            region->ResetMarkBit();
            region = region->GetNextRegion();
        }
    }

    // Recent list regions: just reset mark bits (they are not candidates for collection this cycle)
    region = losRecentLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        region->ResetMarkBit();
        region = region->GetNextRegion();
    }

    losGarbageCollectedBytes = garbageSize;
    DLOG(REGION, "LOS CollectLOSGarbage: reclaimed %zu bytes", garbageSize);
    return garbageSize;
}

size_t LOSManager::ReclaimLOSRegion(RegionInfo* region)
{
    // Mirror RegionManager::CollectRegion but route to losFreeMgr
    size_t regionSize = region->GetRegionSize();
    size_t unitCount = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();

    DLOG(REGION, "LOS reclaim region %p @[0x%zx+%zu, 0x%zx) units[%zu+%zu)",
         region, region->GetRegionStart(), regionSize, region->GetRegionEnd(),
         unitIndex, unitCount);

    region->InitFreeUnits();
    losFreeMgr.AddGarbageUnits(unitIndex, unitCount);
    return regionSize;
}

size_t LOSManager::ReleaseLOSRegion(RegionInfo* region)
{
    // Mirror RegionManager::ReleaseRegion but route to losFreeMgr
    size_t regionSize = region->GetRegionSize();
    size_t unitCount = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();

    DLOG(REGION, "LOS release region %p @[0x%zx+%zu, 0x%zx) units[%zu+%zu)",
         region, region->GetRegionStart(), regionSize, region->GetRegionEnd(),
         unitIndex, unitCount);

    region->InitFreeUnits();
    RegionInfo::ReleaseUnits(unitIndex, unitCount);
    losFreeMgr.AddReleaseUnits(unitIndex, unitCount);
    return regionSize;
}

// ===== M3: MAIN → LOS Migration =====

size_t LOSManager::MigrateLargeObjectsFromMain(RegionList& mainOldList, RegionList& mainGarbageList)
{
    // Clear any previous migration state
    migrationMap.clear();
    size_t migratedCount = 0;
    size_t migratedBytes = 0;

    RegionInfo* region = mainOldList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();

        // Skip pinned objects (have raw pointers — cannot be moved)
        if (region->GetRawPointerObjectCount() > 0) {
            region = next;
            continue;
        }

        // Get the object in this large region (offset 0)
        BaseObject* obj = reinterpret_cast<BaseObject*>(region->GetRegionStart() + RegionSpace::HEADER_SIZE);
        size_t objSize = obj->GetSize();
        size_t allocSize = RegionSpace::ToAllocSize(objSize);

        // Try to allocate equivalent space in LOS
        uintptr_t newAddr = AllocLarge(allocSize);
        if (newAddr == 0) {
            // LOS full — stop migration for this cycle
            DLOG(REGION, "LOS migration: LOS full, stopping. Migrated %zu objects, %zu bytes",
                 migratedCount, migratedBytes);
            break;
        }

        // Copy object data from MAIN region to LOS region
        uintptr_t oldObjAddr = region->GetRegionStart() + RegionSpace::HEADER_SIZE;
        uintptr_t newObjAddr = newAddr;  // AllocLarge returns the object address (after header)
        // Actually AllocLarge returns result of region->Alloc(size) which includes HEADER_SIZE offset
        // So newAddr already points to the object start

        CHECK_E(memmove_s(reinterpret_cast<void*>(newObjAddr), allocSize,
                          reinterpret_cast<void*>(oldObjAddr), allocSize) != EOK,
                "LOS migration: memmove_s failed");

        DLOG(REGION, "LOS migration: obj %p (%zu bytes) from MAIN @0x%zx to LOS @0x%zx",
             obj, allocSize, oldObjAddr, newObjAddr);

        // Record old → new mapping for reference update
        migrationMap.push_back({oldObjAddr, newObjAddr});

        // Remove old MAIN region from old list and put it into garbage for reclaim
        mainOldList.DeleteRegion(region);
        mainGarbageList.PrependRegion(region, RegionInfo::RegionType::GARBAGE_REGION);

        migratedCount++;
        migratedBytes += allocSize;
        region = next;
    }

    losMigrateInCount += migratedCount;
    losMigrateInBytes += migratedBytes;

    DLOG(REPORT, "LOS migration complete: %zu objects, %zu bytes migrated from MAIN to LOS",
         migratedCount, migratedBytes);
    return migratedCount;
}

void LOSManager::UpdateMigratedReferences()
{
    if (migrationMap.empty()) {
        return;
    }

    DLOG(REGION, "LOS migration: updating references for %zu migrated objects", migrationMap.size());

    // Build a lambda that checks if a reference points to a migrated object and updates it.
    // During STW, no concurrent mutation — direct writes are safe.
    auto updateRefField = [this](RefField<>& field) {
        uintptr_t addr = field.GetAddress();
        if (addr == 0) {
            return;
        }
        // Check if this reference points to any migrated object
        for (const auto& mapping : migrationMap) {
            if (addr == mapping.first) {
                // Update reference to point to new LOS location
                BaseObject* newObj = reinterpret_cast<BaseObject*>(mapping.second);
                field.SetTargetObject(newObj);
                DLOG(REGION, "LOS migration: updated ref 0x%zx -> 0x%zx", mapping.first, mapping.second);
                break;
            }
        }
    };

    // 1. Scan all heap objects and update their reference fields
    Heap::GetHeap().ForEachObj([&updateRefField](BaseObject* obj) {
        obj->ForEachRefField(updateRefField);
    }, false);

    // 2. Scan static roots
    Heap::GetHeap().VisitStaticRoots(updateRefField);

    DLOG(REGION, "LOS migration: reference update complete");
}

} // namespace MapleRuntime
