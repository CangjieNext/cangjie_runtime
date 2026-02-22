// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Allocator/LOSManager.h"

#include <algorithm>
#include <unordered_set>

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
        losAllocFailCount.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // A-STEP1: During compaction freeze window, LOS is read/reclaim only.
    // New large allocations are redirected to MAIN by returning failure here.
    if (losCompactionFreeze.load(std::memory_order_acquire)) {
        losAllocFailCount.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    size_t unitCount = (size + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;

    // Helper: common post-allocation steps for all successful paths
    auto finishAllocation = [this, size](RegionInfo* region, const char* source) -> uintptr_t {
        uintptr_t addr = region->Alloc(size);
        DLOG(REGION, "LOS alloc large from %s: region @[0x%zx+%zu, 0x%zx) size %zu",
             source, region->GetRegionStart(), region->GetRegionSize(), region->GetRegionEnd(), size);
        EnlistLargeRegion(region);
        losAllocCount.fetch_add(1, std::memory_order_relaxed);
        return addr;
    };

    // Step 1: Try losFreeMgr (recycled regions from dirtyTree / releaseTree)
    RegionInfo* region = losFreeMgr.TakeRegion(unitCount, RegionInfo::UnitRole::LARGE_SIZED_UNITS, false);
    if (region != nullptr) {
        return finishAllocation(region, "freeMgr");
    }

    // Step 2: Expand losInactiveZone toward low address
    region = AllocFromInactiveZone(unitCount);
    if (region != nullptr) {
        return finishAllocation(region, "inactiveZone");
    }

    // Step 3: Borrow space from RESERVE zone (chunk strategy handled internally by ReserveManager)
    if (reserveManager != nullptr) {
        size_t regionSize = unitCount * RegionInfo::UNIT_SIZE;
        uintptr_t borrowedAddr = reserveManager->BorrowForLOS(regionSize);
        if (borrowedAddr != 0) {
            region = RegionInfo::InitRegionAt(borrowedAddr, unitCount,
                                              RegionInfo::UnitRole::LARGE_SIZED_UNITS);
            losReserveBorrowCount.fetch_add(1, std::memory_order_relaxed);
            return finishAllocation(region, "RESERVE");
        }
    }

    DLOG(REGION, "LOS alloc large failed: size %zu (%zu units), "
         "freeMgr dirty=%u released=%u, inactiveZone=0x%zx, losStart=0x%zx",
         size, unitCount,
         losFreeMgr.GetDirtyUnitCount(), losFreeMgr.GetReleasedUnitCount(),
         losInactiveZone.load(std::memory_order_relaxed), losStart);

    losAllocFailCount.fetch_add(1, std::memory_order_relaxed);
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

    std::unordered_set<uintptr_t> protectedTargetRegionStarts;
    {
        std::lock_guard<std::mutex> lock(postGcMigrationMutex);
        protectedTargetRegionStarts.reserve(postGcInstalledRoutes.size());
        for (const auto& it : postGcInstalledRoutes) {
            uintptr_t newObjAddr = it.second;
            if (newObjAddr == 0) {
                continue;
            }
            protectedTargetRegionStarts.insert(newObjAddr - RegionSpace::HEADER_SIZE);
        }
    }
    {
        std::lock_guard<std::mutex> lock(compactionPlanMutex);
        protectedTargetRegionStarts.reserve(protectedTargetRegionStarts.size() + pendingCompactionPlan.size());
        for (const auto& entry : pendingCompactionPlan) {
            protectedTargetRegionStarts.insert(entry.newStart);
        }
    }

    RegionInfo* region = losOldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        if (protectedTargetRegionStarts.find(region->GetRegionStart()) != protectedTargetRegionStarts.end()) {
            region->ResetMarkBit();
            region = region->GetNextRegion();
            continue;
        }

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

    if (region->IsFreeRegion()) {
        DLOG(REGION, "LOS reclaim skip duplicate free units[%zu+%zu)", unitIndex, unitCount);
        return regionSize;
    }

    region->InitFreeUnits();

    // M5: If region is in RESERVE-borrowed territory (below losStart), defer for return to RESERVE
    if (IsInReserveTerritory(region->GetRegionStart())) {
        pendingReserveReturns.push_back({unitIndex, unitCount});
        DLOG(REGION, "LOS reclaim: RESERVE-borrowed region deferred for return, units[%zu+%zu)",
             unitIndex, unitCount);
        return regionSize;
    }

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

    if (region->IsFreeRegion()) {
        DLOG(REGION, "LOS release skip duplicate free units[%zu+%zu)", unitIndex, unitCount);
        return regionSize;
    }

    region->InitFreeUnits();
    RegionInfo::ReleaseUnits(unitIndex, unitCount);

    // M5: If region is in RESERVE-borrowed territory (below losStart), defer for return to RESERVE
    if (IsInReserveTerritory(region->GetRegionStart())) {
        pendingReserveReturns.push_back({unitIndex, unitCount});
        DLOG(REGION, "LOS release: RESERVE-borrowed region deferred for return, units[%zu+%zu)",
             unitIndex, unitCount);
        return regionSize;
    }

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

        // Copy object data from MAIN region to LOS region.
        // AllocLarge returns region->Alloc(size), which starts at regionStart + HEADER_SIZE.
        // Since HEADER_SIZE == 0, Alloc() returns regionStart directly.
        // oldObjAddr likewise = regionStart + HEADER_SIZE = regionStart.
        static_assert(RegionSpace::HEADER_SIZE == 0,
                      "If HEADER_SIZE becomes non-zero, migration address calculation must be revisited");
        uintptr_t oldObjAddr = region->GetRegionStart() + RegionSpace::HEADER_SIZE;
        uintptr_t newObjAddr = newAddr;

        CHECK_E(memmove_s(reinterpret_cast<void*>(newObjAddr), allocSize,
                          reinterpret_cast<void*>(oldObjAddr), allocSize) != EOK,
                "LOS migration: memmove_s failed");

        DLOG(REGION, "LOS migration: obj %p (%zu bytes) from MAIN @0x%zx to LOS @0x%zx",
             obj, allocSize, oldObjAddr, newObjAddr);

        // Record old → new mapping for reference update
        migrationMap[oldObjAddr] = newObjAddr;

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

size_t LOSManager::MigrateFallbackLargeObjectsFromMain(const std::vector<uintptr_t>& sourceObjAddrs,
                                                       RegionList& mainOldList,
                                                       RegionList& mainGarbageList)
{
    size_t migratedCount = 0;
    size_t migratedBytes = 0;

    std::unordered_set<RegionInfo*> oldLargeRegions;
    RegionInfo* cur = mainOldList.GetHeadRegion();
    while (cur != nullptr) {
        oldLargeRegions.insert(cur);
        cur = cur->GetNextRegion();
    }

    for (uintptr_t oldObjAddr : sourceObjAddrs) {
        if (oldObjAddr == 0) {
            continue;
        }

        RegionInfo* region = RegionInfo::GetRegionInfoAt(oldObjAddr);
        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
            continue;
        }
        if (oldLargeRegions.find(region) == oldLargeRegions.end()) {
            continue;
        }

        if (region->GetRawPointerObjectCount() > 0) {
            continue;
        }

        BaseObject* obj = reinterpret_cast<BaseObject*>(region->GetRegionStart() + RegionSpace::HEADER_SIZE);
        size_t objSize = obj->GetSize();
        size_t allocSize = RegionSpace::ToAllocSize(objSize);

        uintptr_t newAddr = AllocLarge(allocSize);
        if (newAddr == 0) {
            DLOG(REGION, "LOS fallback migration: LOS full, stopping. Migrated %zu objects, %zu bytes",
                 migratedCount, migratedBytes);
            break;
        }

        static_assert(RegionSpace::HEADER_SIZE == 0,
                      "If HEADER_SIZE becomes non-zero, migration address calculation must be revisited");
        uintptr_t oldAddr = region->GetRegionStart() + RegionSpace::HEADER_SIZE;
        uintptr_t newObjAddr = newAddr;

        CHECK_E(memmove_s(reinterpret_cast<void*>(newObjAddr), allocSize,
                          reinterpret_cast<void*>(oldAddr), allocSize) != EOK,
                "LOS fallback migration: memmove_s failed");

        migrationMap[oldAddr] = newObjAddr;

        mainOldList.DeleteRegion(region);
        mainGarbageList.PrependRegion(region, RegionInfo::RegionType::GARBAGE_REGION);
        oldLargeRegions.erase(region);

        migratedCount++;
        migratedBytes += allocSize;
    }

    losMigrateInCount += migratedCount;
    losMigrateInBytes += migratedBytes;

    DLOG(REPORT, "LOS fallback migration complete: %zu objects, %zu bytes migrated from MAIN to LOS",
         migratedCount, migratedBytes);
    return migratedCount;
}

void LOSManager::AddPostGcMigrationCandidates(const std::vector<uintptr_t>& sourceObjAddrs)
{
    if (sourceObjAddrs.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(postGcMigrationMutex);
    for (uintptr_t addr : sourceObjAddrs) {
        if (addr == 0) {
            continue;
        }
        if (postGcInstalledRoutes.find(addr) != postGcInstalledRoutes.end()) {
            continue;
        }
        postGcPendingCandidates.insert(addr);
    }
}

size_t LOSManager::InstallPostGcMainMigrationRoutes(RegionList& mainOldList, RegionList& mainGarbageList)
{
    (void)mainGarbageList;
    std::vector<uintptr_t> candidates;
    {
        std::lock_guard<std::mutex> lock(postGcMigrationMutex);
        if (postGcPendingCandidates.empty()) {
            return 0;
        }
        candidates.reserve(postGcPendingCandidates.size());
        for (uintptr_t addr : postGcPendingCandidates) {
            candidates.push_back(addr);
        }
    }

    std::unordered_set<uintptr_t> oldLargeStarts;
    for (RegionInfo* region = mainOldList.GetHeadRegion(); region != nullptr; region = region->GetNextRegion()) {
        oldLargeStarts.insert(region->GetRegionStart());
    }

    size_t installedCount = 0;
    std::vector<uintptr_t> consumedCandidates;
    consumedCandidates.reserve(candidates.size());
    bool stopCurrentRoundByLosFull = false;

    for (uintptr_t oldObjAddr : candidates) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(oldObjAddr);
        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
            consumedCandidates.push_back(oldObjAddr);
            continue;
        }

        if (oldLargeStarts.find(region->GetRegionStart()) == oldLargeStarts.end()) {
            continue;
        }

        if (region->GetRawPointerObjectCount() > 0) {
            consumedCandidates.push_back(oldObjAddr);
            continue;
        }

        // Defensive recovery for stale route metadata from previous rounds.
        RegionInfo::RouteState routeState = region->GetRouteState();
        if (routeState != RegionInfo::RouteState::NORMAL) {
            if (region->IsGhostFromRegion()) {
                region->DispelGhostFromRegion();
            }
            region->SetRouteState(RegionInfo::RouteState::NORMAL);
            consumedCandidates.push_back(oldObjAddr);
            continue;
        }

        if (region->IsGhostFromRegion()) {
            consumedCandidates.push_back(oldObjAddr);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(postGcMigrationMutex);
            if (postGcInstalledRoutes.find(oldObjAddr) != postGcInstalledRoutes.end()) {
                consumedCandidates.push_back(oldObjAddr);
                continue;
            }
        }

        size_t allocSize = region->GetRegionAllocatedSize();
        if (allocSize == 0 || allocSize > region->GetRegionSize()) {
            consumedCandidates.push_back(oldObjAddr);
            continue;
        }
        uintptr_t newObjAddr = AllocLarge(allocSize);
        if (newObjAddr == 0) {
            // LOS 容量不足：本轮停止后续迁移尝试，未处理候选保留到下一轮 GC。
            stopCurrentRoundByLosFull = true;
            break;
        }

        region->PrepareForwardableLargeRegion(newObjAddr);
        migrationMap[oldObjAddr] = newObjAddr;

        {
            std::lock_guard<std::mutex> lock(postGcMigrationMutex);
            postGcInstalledRoutes[oldObjAddr] = newObjAddr;
            postGcInstalledOldRegionStarts.insert(region->GetRegionStart());
            postGcFinalizeReady = false;
        }

        consumedCandidates.push_back(oldObjAddr);
        ++installedCount;
    }

    if (!consumedCandidates.empty()) {
        std::lock_guard<std::mutex> lock(postGcMigrationMutex);
        for (uintptr_t addr : consumedCandidates) {
            postGcPendingCandidates.erase(addr);
        }
    }

    if (installedCount > 0) {
        DLOG(REPORT, "LOS post-gc migration routes installed: %zu", installedCount);
    }
    if (stopCurrentRoundByLosFull) {
        DLOG(REPORT, "LOS post-gc migration stops this GC round due to LOS full; pending=%zu",
             GetPendingPostGcMainMigrationCount());
    }
    return installedCount;
}

size_t LOSManager::FinalizePostGcMainMigrations(RegionList& mainOldList, RegionList& mainGarbageList)
{
    std::vector<uintptr_t> regionStarts;
    {
        std::lock_guard<std::mutex> lock(postGcMigrationMutex);
        if (postGcInstalledOldRegionStarts.empty()) {
            return 0;
        }
        if (!postGcFinalizeReady) {
            postGcFinalizeReady = true;
            DLOG(REPORT, "LOS post-gc migration finalize delayed by one GC cycle, pending=%zu",
                 postGcInstalledOldRegionStarts.size());
            return 0;
        }
        regionStarts.reserve(postGcInstalledOldRegionStarts.size());
        for (uintptr_t start : postGcInstalledOldRegionStarts) {
            regionStarts.push_back(start);
        }
    }

    std::unordered_set<uintptr_t> oldLargeStarts;
    for (RegionInfo* region = mainOldList.GetHeadRegion(); region != nullptr; region = region->GetNextRegion()) {
        oldLargeStarts.insert(region->GetRegionStart());
    }

    size_t reclaimedCount = 0;
    std::vector<uintptr_t> doneStarts;
    doneStarts.reserve(regionStarts.size());

    for (uintptr_t regionStart : regionStarts) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionStart);
        uintptr_t oldObjAddr = regionStart + RegionSpace::HEADER_SIZE;

        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
            migrationMap.erase(oldObjAddr);
            doneStarts.push_back(regionStart);
            continue;
        }

        if (oldLargeStarts.find(regionStart) == oldLargeStarts.end()) {
            if (region->IsGhostFromRegion()) {
                region->DispelGhostFromRegion();
            }
            migrationMap.erase(oldObjAddr);
            doneStarts.push_back(regionStart);
            continue;
        }

        BaseObject* oldObj = reinterpret_cast<BaseObject*>(oldObjAddr);
        if (!oldObj->IsForwarded()) {
            continue;
        }

        if (region->IsGhostFromRegion()) {
            region->DispelGhostFromRegion();
        }

        mainOldList.DeleteRegion(region);
        mainGarbageList.PrependRegion(region, RegionInfo::RegionType::GARBAGE_REGION);
        migrationMap.erase(oldObjAddr);
        doneStarts.push_back(regionStart);
        ++reclaimedCount;
    }

    if (!doneStarts.empty()) {
        std::lock_guard<std::mutex> lock(postGcMigrationMutex);
        for (uintptr_t start : doneStarts) {
            postGcInstalledOldRegionStarts.erase(start);
            uintptr_t oldObjAddr = start + RegionSpace::HEADER_SIZE;
            postGcInstalledRoutes.erase(oldObjAddr);
        }
    }

    if (reclaimedCount > 0) {
        DLOG(REPORT, "LOS post-gc migration finalized: reclaimed=%zu", reclaimedCount);
    }
    return reclaimedCount;
}

bool LOSManager::HasPendingPostGcMainMigrations() const
{
    std::lock_guard<std::mutex> lock(postGcMigrationMutex);
    return !postGcPendingCandidates.empty() || !postGcInstalledRoutes.empty();
}

size_t LOSManager::GetPendingPostGcMainMigrationCount() const
{
    std::lock_guard<std::mutex> lock(postGcMigrationMutex);
    return postGcPendingCandidates.size() + postGcInstalledRoutes.size();
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
        // O(1) lookup in unordered_map
        auto it = migrationMap.find(addr);
        if (it != migrationMap.end()) {
            // Update reference to point to new LOS location
            BaseObject* newObj = reinterpret_cast<BaseObject*>(it->second);
            field.SetTargetObject(newObj);
            DLOG(REGION, "LOS migration: updated ref 0x%zx -> 0x%zx", it->first, it->second);
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
    if (losAllocFailCount.load(std::memory_order_relaxed) > lastCompactAllocFailCount) {
        return true;
    }

    return false;
}

bool LOSManager::BuildCompactionPlan(std::vector<CompactEntry>& outPlan)
{
    outPlan.clear();

    // Collect all live regions from old/recent lists and split movable vs pinned.
    // Must iterate under RegionList lock to avoid concurrent list mutation races.
    std::vector<RegionInfo*> movableRegions;
    uintptr_t highestPinnedEnd = losInactiveZone.load(std::memory_order_relaxed);

    auto collectRegions = [&](RegionList& list) {
        list.VisitAllRegions([&](RegionInfo* r) {
            if (r->GetRawPointerObjectCount() > 0) {
                uintptr_t rEnd = r->GetRegionEnd();
                if (rEnd > highestPinnedEnd) {
                    highestPinnedEnd = rEnd;
                }
            } else {
                movableRegions.push_back(r);
            }
        });
    };

    collectRegions(losOldLargeRegionList);
    collectRegions(losRecentLargeRegionList);

    if (movableRegions.empty()) {
        return false;
    }

    // Sort movable regions high->low, then assign targets from losEnd downward.
    std::sort(movableRegions.begin(), movableRegions.end(),
              [](RegionInfo* a, RegionInfo* b) { return a->GetRegionStart() > b->GetRegionStart(); });

    uintptr_t cursor = losEnd;
    for (RegionInfo* r : movableRegions) {
        size_t regionSize = r->GetRegionSize();
        uintptr_t newStart = cursor - regionSize;

        // Strategy A: do not cross highest pinned barrier.
        if (newStart < highestPinnedEnd) {
            break;
        }

        if (newStart != r->GetRegionStart()) {
            CompactEntry entry;
            entry.oldStart = r->GetRegionStart();
            entry.newStart = newStart;
            entry.regionSize = regionSize;
            entry.unitCount = r->GetUnitCount();
            outPlan.push_back(entry);
        }
        cursor = newStart;
    }

    return !outPlan.empty();
}

bool LOSManager::InstallPendingCompactionForForwarding()
{
    std::lock_guard<std::mutex> lock(compactionPlanMutex);
    if (pendingCompactionPlan.empty()) {
        return false;
    }

    size_t installedCount = 0;
    size_t newlyInstalledCount = 0;
    size_t unresolvedCount = 0;
    for (const auto& entry : pendingCompactionPlan) {
        bool wasInstalled = IsCompactionEntryInstalledLocked(entry);
        bool wasResolved = IsCompactionEntryResolvedLocked(entry);
        std::unordered_set<uintptr_t> visiting;
        if (InstallCompactionEntryWithPredecessorsLocked(entry, visiting)) {
            ++installedCount;
            if (!wasInstalled && !wasResolved && IsCompactionEntryInstalledLocked(entry)) {
                ++newlyInstalledCount;
            }
        } else {
            ++unresolvedCount;
        }
    }

    DLOG(REPORT,
         "LOS install compaction forwarding: total=%zu installed=%zu newlyInstalled=%zu unresolved=%zu",
         pendingCompactionPlan.size(), installedCount, newlyInstalledCount, unresolvedCount);
    return installedCount > 0;
}

size_t LOSManager::FinalizePendingCompaction()
{
    std::lock_guard<std::mutex> lock(compactionPlanMutex);
    if (pendingCompactionPlan.empty()) {
        return 0;
    }

    size_t finalizedCount = 0;
    size_t pendingCount = 0;
    size_t waitingForwardCount = 0;
    std::vector<RegionInfo*> newRegions;
    newRegions.reserve(pendingCompactionPlan.size());
    std::vector<CompactEntry> remainingPlan;
    remainingPlan.reserve(pendingCompactionPlan.size());

    for (const auto& entry : pendingCompactionPlan) {
        RegionInfo* oldRegion = RegionInfo::GetRegionInfoAt(entry.oldStart);
        if (oldRegion == nullptr || !oldRegion->IsGhostFromRegion()) {
            remainingPlan.push_back(entry);
            ++pendingCount;
            continue;
        }

        BaseObject* oldObj = reinterpret_cast<BaseObject*>(entry.oldStart + RegionSpace::HEADER_SIZE);
        if (!oldObj->IsForwarded()) {
            remainingPlan.push_back(entry);
            ++pendingCount;
            ++waitingForwardCount;
            continue;
        }

        // Remove old region from large lists first (before metadata reset).
        // Use robust dual-list deletion to tolerate stale regionType transitions.
        losOldLargeRegionList.DeleteRegion(oldRegion);
        losRecentLargeRegionList.DeleteRegion(oldRegion);

        // Build destination region metadata at compacted address.
        RegionInfo* newRegion = RegionInfo::InitRegionAt(entry.newStart, entry.unitCount,
                                                         RegionInfo::UnitRole::LARGE_SIZED_UNITS);
        (void)newRegion->Alloc(entry.regionSize - RegionSpace::HEADER_SIZE);
        newRegions.push_back(newRegion);

        // Reclaim old region to LOS free manager.
        oldRegion->DispelGhostFromRegion();
        if (oldRegion->IsFreeRegion()) {
            DLOG(REPORT, "LOS finalize skip reclaim: region already free [%zu+%zu)",
                 oldRegion->GetUnitIdx(), oldRegion->GetUnitCount());
        } else {
            (void)ReclaimLOSRegion(oldRegion);
        }

        migrationMap.erase(entry.oldStart + RegionSpace::HEADER_SIZE);

        ++finalizedCount;
    }

    for (RegionInfo* newRegion : newRegions) {
        losOldLargeRegionList.PrependRegion(newRegion, RegionInfo::RegionType::LARGE_REGION);
    }

    pendingCompactionPlan.swap(remainingPlan);
    if (pendingCompactionPlan.empty()) {
        migrationMap.clear();
    }

    DLOG(REPORT, "LOS finalize compaction: finalized=%zu pending=%zu waitingForward=%zu",
         finalizedCount, pendingCount, waitingForwardCount);
    return finalizedCount;
}

bool LOSManager::EnsureCompactionRouteForObject(uintptr_t objAddr)
{
    if (!IsCompacting()) {
        return false;
    }
    if (RegionInfo::GetGhostFromRegionAt(static_cast<MAddress>(objAddr)) != nullptr) {
        return true;
    }

    std::lock_guard<std::mutex> lock(compactionPlanMutex);
    if (pendingCompactionPlan.empty()) {
        return false;
    }

    for (const auto& entry : pendingCompactionPlan) {
        uintptr_t oldObjStart = entry.oldStart + RegionSpace::HEADER_SIZE;
        uintptr_t oldObjEnd = entry.oldStart + entry.regionSize;
        if (objAddr < oldObjStart || objAddr >= oldObjEnd) {
            continue;
        }
        std::unordered_set<uintptr_t> visiting;
        return InstallCompactionEntryWithPredecessorsLocked(entry, visiting);
    }

    return false;
}

bool LOSManager::IsCompactionEntryInstalledLocked(const CompactEntry& entry) const
{
    RegionInfo* oldRegion = RegionInfo::GetRegionInfoAt(entry.oldStart);
    return oldRegion != nullptr && oldRegion->IsGhostFromRegion();
}

bool LOSManager::IsCompactionEntryResolvedLocked(const CompactEntry& entry) const
{
    RegionInfo* oldRegion = RegionInfo::GetRegionInfoAt(entry.oldStart);
    if (oldRegion == nullptr || !oldRegion->IsGhostFromRegion()) {
        return false;
    }

    BaseObject* oldObj = reinterpret_cast<BaseObject*>(entry.oldStart + RegionSpace::HEADER_SIZE);
    return oldObj->IsForwarded();
}

bool LOSManager::InstallCompactionEntryLocked(const CompactEntry& entry)
{
    RegionInfo* oldRegion = RegionInfo::GetRegionInfoAt(entry.oldStart);
    if (oldRegion == nullptr) {
        return false;
    }
    if (oldRegion->IsGhostFromRegion()) {
        return true;
    }

    uintptr_t oldObjAddr = entry.oldStart + RegionSpace::HEADER_SIZE;
    uintptr_t newObjAddr = entry.newStart + RegionSpace::HEADER_SIZE;
    oldRegion->PrepareForwardableLargeRegion(newObjAddr);
    migrationMap[oldObjAddr] = newObjAddr;
    return true;
}

bool LOSManager::InstallCompactionEntryWithPredecessorsLocked(const CompactEntry& entry,
                                                              std::unordered_set<uintptr_t>& visiting)
{
    if (IsCompactionEntryResolvedLocked(entry) || IsCompactionEntryInstalledLocked(entry)) {
        return true;
    }

    if (!visiting.insert(entry.oldStart).second) {
        DLOG(REPORT, "LOS compaction dependency cycle detected at 0x%zx", entry.oldStart);
        return false;
    }

    uintptr_t targetStart = entry.newStart;
    uintptr_t targetEnd = entry.newStart + entry.regionSize;

    for (const auto& other : pendingCompactionPlan) {
        if (other.oldStart == entry.oldStart) {
            continue;
        }
        if (IsCompactionEntryResolvedLocked(other)) {
            continue;
        }

        uintptr_t sourceStart = other.oldStart;
        uintptr_t sourceEnd = other.oldStart + other.regionSize;
        if (!RangesOverlap(targetStart, targetEnd, sourceStart, sourceEnd)) {
            continue;
        }

        if (!InstallCompactionEntryWithPredecessorsLocked(other, visiting)) {
            visiting.erase(entry.oldStart);
            return false;
        }
    }

    bool ok = InstallCompactionEntryLocked(entry);
    visiting.erase(entry.oldStart);
    return ok;
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
        lastCompactAllocFailCount = losAllocFailCount.load(std::memory_order_relaxed);
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
        lastCompactAllocFailCount = losAllocFailCount.load(std::memory_order_relaxed);
        return 0;
    }

    // Phase 3: Build forwarding map for reference update.
    // Object address = region start + HEADER_SIZE (large region has one object).
    migrationMap.reserve(migrationMap.size() + compactPlan.size());
    for (const auto& entry : compactPlan) {
        uintptr_t oldObjAddr = entry.oldStart + RegionSpace::HEADER_SIZE;
        uintptr_t newObjAddr = entry.newStart + RegionSpace::HEADER_SIZE;
        migrationMap[oldObjAddr] = newObjAddr;
        DLOG(REGION, "LOS compact: plan move obj @0x%zx → @0x%zx (%zu bytes)",
             oldObjAddr, newObjAddr, entry.regionSize);
    }

    // Phase 4: Reference update is DEFERRED to WCollector.
    // CompactLOS only builds the forwarding map; WCollector calls
    // UpdateMigratedReferences() once after both compaction and migration
    // entries are collected, saving a full heap scan.

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
            // Overlap defense: memmove_s handles overlaps correctly, but assert
            // our sliding compaction invariant — objects slide toward high address,
            // so newStart > oldStart (no backward moves in Strategy A).
            CHECK_E(entry.newStart <= entry.oldStart,
                    "LOS compact: unexpected backward move @0x%zx → 0x%zx (size %zu). "
                    "Sliding compaction should only move toward high address.",
                    entry.oldStart, entry.newStart, entry.regionSize);

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
    // Build a set for O(1) lookup instead of O(n²) nested loop.
    std::unordered_set<RegionInfo*> compactPlanSet;
    compactPlanSet.reserve(compactPlan.size());
    for (const auto& entry : compactPlan) {
        compactPlanSet.insert(entry.region);
    }
    for (RegionInfo* r : movableRegions) {
        if (compactPlanSet.count(r) == 0) {
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
    lastCompactAllocFailCount = losAllocFailCount.load(std::memory_order_relaxed);

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
    // The space between old and new losInactiveZone is now unused LOS space.
    // With hole bitmap, ReturnFromLOS always succeeds (marks bitmap + coalesces watermark).
    uintptr_t returnAddr = inactiveZone;  // new losInactiveZone (= lowest live object)
    reserveManager->ReturnFromLOS(returnAddr, returnSize);
    DLOG(REPORT, "LOS ShrinkToReserve: returned %zu bytes @0x%zx to RESERVE", returnSize, returnAddr);
    return returnSize;
}

// ===== M5: Return RESERVE-borrowed free LOS regions to RESERVE =====

size_t LOSManager::ReturnBorrowedToReserve()
{
    if (reserveManager == nullptr || pendingReserveReturns.empty()) {
        return 0;
    }

    std::sort(pendingReserveReturns.begin(), pendingReserveReturns.end());
    pendingReserveReturns.erase(std::unique(pendingReserveReturns.begin(), pendingReserveReturns.end()),
                               pendingReserveReturns.end());

    // M5 hysteresis: skip proactive return if LOS return is in cooldown
    // (unless there are many pending returns, indicating heavy return pressure)
    if (reserveManager->IsLOSReturnInCooldown() && pendingReserveReturns.size() < 4) {
        // Still in cooldown — put pending returns into losFreeMgr for reuse instead
        for (auto& entry : pendingReserveReturns) {
            losFreeMgr.AddGarbageUnits(entry.first, entry.second);
        }
        DLOG(REGION, "LOS ReturnBorrowedToReserve: cooldown, %zu entries kept in losFreeMgr",
             pendingReserveReturns.size());
        pendingReserveReturns.clear();
        return 0;
    }

    // With hole bitmap, no sorting needed — ReturnFromLOS accepts any position.
    // Bitmap records the return; watermark coalesces automatically when contiguous.
    size_t returnedBytes = 0;
    for (auto& entry : pendingReserveReturns) {
        uintptr_t addr = RegionInfo::GetUnitAddress(entry.first);
        size_t size = entry.second * RegionInfo::UNIT_SIZE;
        reserveManager->ReturnFromLOS(addr, size);
        returnedBytes += size;
        DLOG(REGION, "LOS ReturnBorrowedToReserve: returned %zu bytes @0x%zx", size, addr);
    }

    pendingReserveReturns.clear();

    if (returnedBytes > 0) {
        DLOG(REPORT, "LOS ReturnBorrowedToReserve: returned %zu bytes total", returnedBytes);
    }
    return returnedBytes;
}

} // namespace MapleRuntime
