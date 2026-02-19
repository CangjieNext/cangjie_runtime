// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Allocator/LOSManager.h"

#include <algorithm>

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
    // M4a-async: During compaction, reject all LOS allocations → forces MAIN fallback
    if (isCompacting.load(std::memory_order_acquire)) {
        losAllocFailCount++;
        return 0;
    }

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
    // M4a: DO NOT clear migrationMap here — compaction entries may already be present.
    // Append migration entries so that a single UpdateMigratedReferences() scan handles both.
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

// ===== M4a: LOS Sliding Compaction =====

double LOSManager::GetFragmentationRatio() const
{
    uintptr_t inactiveZone = losInactiveZone.load(std::memory_order_relaxed);
    size_t activeSize = losEnd - inactiveZone;
    if (activeSize == 0) {
        return 0.0;
    }
    size_t freeUnits = losFreeMgr.GetDirtyUnitCount() + losFreeMgr.GetReleasedUnitCount();
    size_t totalActiveUnits = activeSize / RegionInfo::UNIT_SIZE;
    if (totalActiveUnits == 0) {
        return 0.0;
    }
    return static_cast<double>(freeUnits) / static_cast<double>(totalActiveUnits);
}

bool LOSManager::NeedsCompaction() const
{
    uintptr_t inactiveZone = losInactiveZone.load(std::memory_order_relaxed);
    size_t activeSize = losEnd - inactiveZone;
    if (activeSize == 0) {
        return false;
    }

    // Condition 1: Fragmentation ratio exceeds threshold
    if (GetFragmentationRatio() > LOS_COMPACTION_FRAG_THRESHOLD) {
        return true;
    }

    // Condition 2: LOS allocation failures since last compaction
    if (losAllocFailCount > lastCompactAllocFailCount) {
        return true;
    }

    return false;
}

size_t LOSManager::CompactLOS()
{
    DLOG(REPORT, "LOS CompactLOS: starting, fragRatio=%.2f, freeUnits(dirty=%u, released=%u), "
         "inactiveZone=0x%zx, losEnd=0x%zx",
         GetFragmentationRatio(),
         losFreeMgr.GetDirtyUnitCount(), losFreeMgr.GetReleasedUnitCount(),
         losInactiveZone.load(std::memory_order_relaxed), losEnd);

    // Phase 1: Collect all live regions from both old and recent lists.
    // Separate into movable (rawPointerCount == 0) and pinned.
    std::vector<RegionInfo*> movableRegions;
    std::vector<RegionInfo*> pinnedRegions;
    uintptr_t highestPinnedEnd = losInactiveZone.load(std::memory_order_relaxed);  // default: no barrier

    auto collectRegions = [&](RegionList& list) {
        for (RegionInfo* r = list.GetHeadRegion(); r != nullptr; r = r->GetNextRegion()) {
            if (r->GetRawPointerObjectCount() > 0) {
                pinnedRegions.push_back(r);
                uintptr_t rEnd = r->GetRegionEnd();
                if (rEnd > highestPinnedEnd) {
                    highestPinnedEnd = rEnd;
                }
            } else {
                movableRegions.push_back(r);
            }
        }
    };

    collectRegions(losOldLargeRegionList);
    collectRegions(losRecentLargeRegionList);

    if (movableRegions.empty()) {
        DLOG(REPORT, "LOS CompactLOS: no movable regions, skipping");
        losCompactCount++;
        lastCompactAllocFailCount = losAllocFailCount;
        return 0;
    }

    // Sort movable regions by address descending (high → low) for target assignment.
    // We assign targets from losEnd downward.
    std::sort(movableRegions.begin(), movableRegions.end(),
              [](RegionInfo* a, RegionInfo* b) {
                  return a->GetRegionStart() > b->GetRegionStart();
              });

    // Phase 2: Compute target addresses — slide toward losEnd (high address).
    // Strategy A: only compact above highestPinnedEnd.
    uintptr_t cursor = losEnd;
    // forwardPlan: { region, oldAddr, newAddr, regionSize, unitCount }
    struct CompactEntry {
        RegionInfo* region;
        uintptr_t oldStart;
        uintptr_t newStart;
        size_t regionSize;
        size_t unitCount;
    };
    std::vector<CompactEntry> compactPlan;
    size_t movedBytes = 0;

    for (RegionInfo* r : movableRegions) {
        size_t rSize = r->GetRegionSize();
        uintptr_t newStart = cursor - rSize;

        // Don't cross the pinned barrier
        if (newStart < highestPinnedEnd) {
            // Remaining movable regions at lower addresses can't be compacted (Strategy A).
            break;
        }

        if (newStart != r->GetRegionStart()) {
            // This object actually moves
            compactPlan.push_back({r, r->GetRegionStart(), newStart, rSize, r->GetUnitCount()});
            movedBytes += rSize;
        }
        cursor = newStart;
    }

    if (compactPlan.empty()) {
        DLOG(REPORT, "LOS CompactLOS: no regions need to move (already compact)");
        losCompactCount++;
        lastCompactAllocFailCount = losAllocFailCount;
        return 0;
    }

    // Phase 3: Build forwarding map for reference update.
    // Object address = region start + HEADER_SIZE (large region has one object).
    for (const auto& entry : compactPlan) {
        uintptr_t oldObjAddr = entry.oldStart + RegionSpace::HEADER_SIZE;
        uintptr_t newObjAddr = entry.newStart + RegionSpace::HEADER_SIZE;
        migrationMap.push_back({oldObjAddr, newObjAddr});
        DLOG(REGION, "LOS compact: plan move obj @0x%zx → @0x%zx (%zu bytes)",
             oldObjAddr, newObjAddr, entry.regionSize);
    }

    // Phase 4: Update all references (covers compaction forwarding).
    // Note: migrationMap may later also contain M3 migration entries.
    // We update references now for compaction; M3 migration will append and update again if needed.
    UpdateMigratedReferences();

    // Phase 5: Move objects (iterate compactPlan in order: highest target first).
    // compactPlan is already ordered high-to-low (since movableRegions was sorted descending
    // and we assigned targets from losEnd downward; first entry gets highest target).
    // Since all objects slide toward high address (newStart >= oldStart... not necessarily,
    // but objects move toward losEnd), we must ensure no overwrite.
    // The plan assigns targets contiguously from losEnd downward. Objects that were already
    // at high addresses get the highest targets. Moving in plan order (high target first)
    // ensures we write to the highest freshly assigned address first.
    // Within a contiguous pack, memmove_s handles overlap correctly.

    // Detach all movable regions from their lists before moving.
    // We'll rebuild lists from scratch after moving.
    losOldLargeRegionList.ClearList();
    losRecentLargeRegionList.ClearList();

    for (const auto& entry : compactPlan) {
        if (entry.newStart != entry.oldStart) {
            int ret = memmove_s(reinterpret_cast<void*>(entry.newStart), entry.regionSize,
                                reinterpret_cast<void*>(entry.oldStart), entry.regionSize);
            CHECK_E(ret != EOK, "LOS compact: memmove_s failed for region @0x%zx → 0x%zx (%zu bytes)",
                    entry.oldStart, entry.newStart, entry.regionSize);

            // Clear old RegionInfo metadata
            entry.region->InitFreeUnits();

            // Initialize new RegionInfo at target address
            RegionInfo* newRegion = RegionInfo::InitRegionAt(entry.newStart, entry.unitCount,
                                                             RegionInfo::UnitRole::LARGE_SIZED_UNITS);
            // Set allocPtr to reflect the occupied space
            newRegion->Alloc(entry.regionSize - RegionSpace::HEADER_SIZE);  // mark region as having allocated data

            DLOG(REGION, "LOS compact: moved region @0x%zx → @0x%zx (%zu units)",
                 entry.oldStart, entry.newStart, entry.unitCount);
        }
    }

    // Phase 5b: Rebuild region lists.
    // Collect all live regions (moved + pinned + unmoved movable) and re-insert into old list.
    std::vector<RegionInfo*> allLiveRegions;

    // Compacted regions (at new addresses)
    for (const auto& entry : compactPlan) {
        RegionInfo* newRegion = RegionInfo::GetRegionInfoAt(entry.newStart);
        allLiveRegions.push_back(newRegion);
        losOldLargeRegionList.PrependRegion(newRegion, RegionInfo::RegionType::LARGE_REGION);
    }

    // Movable regions that weren't in compactPlan (below pinned barrier, couldn't move)
    for (RegionInfo* r : movableRegions) {
        bool inPlan = false;
        for (const auto& entry : compactPlan) {
            if (entry.region == r) {
                inPlan = true;
                break;
            }
        }
        if (!inPlan) {
            allLiveRegions.push_back(r);
            losOldLargeRegionList.PrependRegion(r, RegionInfo::RegionType::LARGE_REGION);
        }
    }

    // Pinned regions (unchanged position)
    for (RegionInfo* r : pinnedRegions) {
        allLiveRegions.push_back(r);
        losOldLargeRegionList.PrependRegion(r, RegionInfo::RegionType::LARGE_REGION);
    }

    // Phase 6: Rebuild FreeRegionManager and update losInactiveZone.
    // Find the lowest live region start address.
    uintptr_t lowestLiveStart = losEnd;
    for (RegionInfo* r : allLiveRegions) {
        if (r->GetRegionStart() < lowestLiveStart) {
            lowestLiveStart = r->GetRegionStart();
        }
    }

    // Update losInactiveZone to the lowest live object boundary
    uintptr_t oldInactiveZone = losInactiveZone.load(std::memory_order_relaxed);
    losInactiveZone.store(lowestLiveStart, std::memory_order_relaxed);

    // Sort allLiveRegions by address ascending for gap calculation
    std::sort(allLiveRegions.begin(), allLiveRegions.end(),
              [](RegionInfo* a, RegionInfo* b) {
                  return a->GetRegionStart() < b->GetRegionStart();
              });

    // Rebuild losFreeMgr: clear trees, then insert gaps between live regions
    RebuildFreeRegionManager(allLiveRegions);

    // Phase 7: Shrink to RESERVE — return freed space at low address end.
    size_t freedBytes = lowestLiveStart - oldInactiveZone;
    size_t shrunkBytes = ShrinkToReserve(freedBytes);

    // Update statistics
    losCompactCount++;
    losCompactMovedBytes = movedBytes;
    losCompactFreedBytes = freedBytes;
    losCompactShrunkBytes = shrunkBytes;
    lastCompactAllocFailCount = losAllocFailCount;

    DLOG(REPORT, "LOS CompactLOS done: moved %zu bytes (%zu regions), freed %zu bytes, "
         "shrunk %zu bytes to RESERVE, newInactiveZone=0x%zx, live regions=%zu",
         movedBytes, compactPlan.size(), freedBytes, shrunkBytes,
         losInactiveZone.load(std::memory_order_relaxed), allLiveRegions.size());

    return freedBytes;
}

void LOSManager::RebuildFreeRegionManager(const std::vector<RegionInfo*>& allLiveRegions)
{
    // Clear both trees
    losFreeMgr.ClearTrees();

    uintptr_t activeStart = losInactiveZone.load(std::memory_order_relaxed);

    // Iterate through sorted live regions and identify gaps between them.
    // Active LOS range: [activeStart, losEnd)
    // Gaps between live regions within this range should be added to losFreeMgr as dirty units.
    uintptr_t gapStart = activeStart;

    for (RegionInfo* r : allLiveRegions) {
        uintptr_t rStart = r->GetRegionStart();
        if (rStart > gapStart) {
            // There's a gap [gapStart, rStart)
            size_t gapSize = rStart - gapStart;
            size_t gapUnits = gapSize / RegionInfo::UNIT_SIZE;
            if (gapUnits > 0) {
                // Initialize the gap units as free
                RegionInfo* gapRegion = RegionInfo::InitRegionAt(gapStart, gapUnits,
                                                                 RegionInfo::UnitRole::LARGE_SIZED_UNITS);
                gapRegion->InitFreeUnits();
                size_t gapUnitIdx = gapRegion->GetUnitIdx();
                losFreeMgr.AddGarbageUnits(static_cast<CartesianTree::Index>(gapUnitIdx),
                                           static_cast<CartesianTree::Count>(gapUnits));
                DLOG(REGION, "LOS rebuild freeMgr: gap [0x%zx, 0x%zx) = %zu units",
                     gapStart, rStart, gapUnits);
            }
        }
        gapStart = r->GetRegionEnd();
    }

    // Final gap: from last live region end to losEnd — should not exist after compaction
    // (objects are packed toward losEnd), but handle defensively.
    if (gapStart < losEnd) {
        size_t gapSize = losEnd - gapStart;
        size_t gapUnits = gapSize / RegionInfo::UNIT_SIZE;
        if (gapUnits > 0) {
            RegionInfo* gapRegion = RegionInfo::InitRegionAt(gapStart, gapUnits,
                                                             RegionInfo::UnitRole::LARGE_SIZED_UNITS);
            gapRegion->InitFreeUnits();
            size_t gapUnitIdx = gapRegion->GetUnitIdx();
            losFreeMgr.AddGarbageUnits(static_cast<CartesianTree::Index>(gapUnitIdx),
                                       static_cast<CartesianTree::Count>(gapUnits));
            DLOG(REGION, "LOS rebuild freeMgr: trailing gap [0x%zx, 0x%zx) = %zu units",
                     gapStart, losEnd, gapUnits);
        }
    }

    DLOG(REPORT, "LOS rebuild freeMgr: dirty=%u, released=%u",
         losFreeMgr.GetDirtyUnitCount(), losFreeMgr.GetReleasedUnitCount());
}

size_t LOSManager::ShrinkToReserve(size_t freedBytes)
{
    if (freedBytes == 0 || reserveManager == nullptr) {
        return 0;
    }

    // The freed space is at the low-address end of LOS: [old losInactiveZone, new losInactiveZone).
    // But losInactiveZone has already been updated. The freed region is:
    //   [losInactiveZone - freedBytes ... is no longer correct]
    // Actually: losInactiveZone was moved from oldInactiveZone to lowestLiveStart.
    // The freed bytes = lowestLiveStart - oldInactiveZone.
    // The freed address range was [oldInactiveZone, lowestLiveStart).
    // But after compaction, losInactiveZone = lowestLiveStart and the space below
    // [oldInactiveZone, lowestLiveStart) is now unused.
    // However, we cannot directly "return" LOS address space to RESERVE because
    // the LOS boundary (losStart) is fixed. Instead, we shrink LOS by moving
    // losStart upward, effectively returning the low-end to RESERVE.
    //
    // For now: return the freed bytes as a ReturnFromLOS call, which adjusts RESERVE
    // watermarks. The actual LOS range [losStart, losEnd) doesn't change, but
    // the inactive zone has moved up, so the effective capacity decreased.
    //
    // Let RESERVE know about the freed space:
    uintptr_t inactiveZone = losInactiveZone.load(std::memory_order_relaxed);
    size_t returnSize = freedBytes;

    // Align return size to unit boundaries
    returnSize = (returnSize / RegionInfo::UNIT_SIZE) * RegionInfo::UNIT_SIZE;
    if (returnSize == 0) {
        return 0;
    }

    // Return the freed low-address region to RESERVE.
    // The address range [inactiveZone, inactiveZone + freedBytes) is now idle.
    // Actually we return from the old boundary: the space that was between
    // old losInactiveZone and the new one is now unused LOS space.
    uintptr_t returnAddr = inactiveZone;  // new losInactiveZone (= lowest live object)
    // The space below losInactiveZone is already inactive; we're just informing RESERVE.
    reserveManager->ReturnFromLOS(returnAddr, returnSize);

    DLOG(REPORT, "LOS ShrinkToReserve: returned %zu bytes @0x%zx to RESERVE", returnSize, returnAddr);
    return returnSize;
}

} // namespace MapleRuntime
