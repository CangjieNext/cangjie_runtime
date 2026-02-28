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

void LOSManager::Initialize(uintptr_t start, uintptr_t end, UnitCount unitStart, UnitCount unitEnd,
                            UnitCount reserveUnitCount)
{
    losStart = start;
    losEnd = end;
    losUnitStart = unitStart;
    losUnitEnd = unitEnd;

    // Initialize inactive zone at the end (LOS grows toward low address)
    losInactiveZone.store(losEnd, std::memory_order_relaxed);

    // Initialize FreeRegionManager for LOS space.
    // P4: baseUnit = mainUnitCount (set in constructor) so the tree covers RESERVE
    // extension area + native LOS space. Capacity = reserveUnitCount + losUnitCount.
    // This allows RESERVE-borrowed dead regions to be recycled normally.
    UnitCount losUnitCount = unitEnd - unitStart;
    losFreeMgr.Initialize(reserveUnitCount + losUnitCount);

    DLOG(REPORT, "LOSManager initialized: space [0x%zx, 0x%zx), units [%u, %u), capacity %zu bytes, "
         "FreeRegionManager baseUnit=%u, treeCapacity=%u (reserve=%u + los=%u)",
         losStart, losEnd, losUnitStart, losUnitEnd, GetCapacity(), losFreeMgr.GetBaseUnit(),
         reserveUnitCount + losUnitCount, reserveUnitCount, losUnitCount);
}

// ===== M2: Large object allocation =====

uintptr_t LOSManager::AllocLarge(size_t size)
{
    // NOTE: No need to check isCompacting / losCompactionFreeze here.
    // CompactLOS always runs inside ScopedStopTheWorld, so all mutators
    // are paused and cannot reach AllocLarge() during compaction.

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

    // Step 3: Borrow-on-demand from RESERVE, then retry AllocFromInactiveZone.
    // ComputeBorrowChunk inside BorrowForLOS computes max(requestSize, 10% of available).
    if (reserveManager != nullptr) {
        size_t neededSize = unitCount * RegionInfo::UNIT_SIZE;
        size_t borrowed = reserveManager->BorrowForLOS(neededSize);
        if (borrowed > 0) {
            region = AllocFromInactiveZone(unitCount);
            if (region != nullptr) {
                return finishAllocation(region, "reserveBorrow");
            }
        }
    }

    DLOG(REGION, "LOS alloc large failed: size %zu (%zu units), "
         "freeMgr dirty=%u released=%u, inactiveZone=0x%zx, losStart=0x%zx",
         size, unitCount,
         losFreeMgr.GetDirtyUnitCount(), losFreeMgr.GetReleasedUnitCount(),
         losInactiveZone.load(std::memory_order_relaxed), losStart);

    // Track failed allocation size for CompactLOS skip heuristic (#10A).
    // After GC reclaims dead regions, if the free space can satisfy this size,
    // we skip the expensive CompactLOS.
    losLastFailedAllocSize.store(size, std::memory_order_relaxed);
    losAllocFailCount.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

RegionInfo* LOSManager::AllocFromInactiveZone(size_t unitCount)
{
    size_t regionSize = unitCount * RegionInfo::UNIT_SIZE;
    uintptr_t effectiveStart = GetEffectiveLOSStart();
    uintptr_t currentZone = losInactiveZone.load(std::memory_order_relaxed);

    // P5: RESERVE borrowing removed from here. If inactiveZone is insufficient,
    // returns nullptr. The caller's retry loop triggers OOM GC, then borrows
    // from RESERVE at the RegionSpace level before retrying.
    if (currentZone < effectiveStart + regionSize) {
        return nullptr;  // not enough space
    }

    // Try CAS to claim the inactive zone
    uintptr_t newZone = currentZone - regionSize;
    while (!losInactiveZone.compare_exchange_weak(currentZone, newZone,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
        if (currentZone < effectiveStart + regionSize) {
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

    // P2-opt: Use inline array + linear scan instead of unordered_set.
    // pendingCompactionPlan is typically 0-10 entries; linear scan is faster than hash table.
    static constexpr size_t INLINE_PROTECTED_SIZE = 32;
    uintptr_t protectedStarts[INLINE_PROTECTED_SIZE];
    size_t protectedCount = 0;
    {
        std::lock_guard<std::mutex> lock(compactionPlanMutex);
        for (const auto& entry : pendingCompactionPlan) {
            if (protectedCount < INLINE_PROTECTED_SIZE) {
                protectedStarts[protectedCount++] = entry.newStart;
            }
        }
    }

    auto isProtected = [&](uintptr_t start) -> bool {
        for (size_t i = 0; i < protectedCount; i++) {
            if (protectedStarts[i] == start) return true;
        }
        return false;
    };

    RegionInfo* region = losOldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        if (isProtected(region->GetRegionStart())) {
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

            // P0-opt: Always release (madvise/mmap) and route to releasedTree,
            // so TakeRegion avoids ClearUnits memset on the allocation hot path.
            garbageSize += ReleaseLOSRegion(del);
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

    losFreeMgr.AddReleaseUnits(unitIndex, unitCount);
    return regionSize;
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

bool LOSManager::CanAllocLargeAfterGC() const
{
    // #10A: Check if LOS has enough free space to satisfy the last failed allocation.
    // Called after CollectLargeGarbage() has reclaimed dead regions, before CompactLOS.
    size_t neededSize = losLastFailedAllocSize.load(std::memory_order_relaxed);
    if (neededSize == 0) {
        return true;  // no recent failure — no need to compact
    }

    size_t unitCount = (neededSize + RegionInfo::UNIT_SIZE - 1) / RegionInfo::UNIT_SIZE;

    // Check 1: losFreeMgr has a contiguous free block large enough
    if (losFreeMgr.GetMaxContiguousFreeUnits() >= unitCount) {
        DLOG(REPORT, "CanAllocLargeAfterGC: freeMgr has %u contiguous units >= needed %zu",
             losFreeMgr.GetMaxContiguousFreeUnits(), unitCount);
        return true;
    }

    // Check 2: inactive zone runway is large enough
    uintptr_t iz = losInactiveZone.load(std::memory_order_relaxed);
    uintptr_t effectiveStart = GetEffectiveLOSStart();
    size_t runway = (iz > effectiveStart) ? (iz - effectiveStart) : 0;
    if (runway >= neededSize) {
        DLOG(REPORT, "CanAllocLargeAfterGC: inactiveZone runway %zu >= needed %zu",
             runway, neededSize);
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

        // P0-opt: Release old region (madvise/mmap → releasedTree) to avoid
        // ClearUnits memset when the region is reused by TakeRegion.
        oldRegion->DispelGhostFromRegion();
        if (oldRegion->IsFreeRegion()) {
            DLOG(REPORT, "LOS finalize skip reclaim: region already free [%zu+%zu)",
                 oldRegion->GetUnitIdx(), oldRegion->GetUnitCount());
        } else {
            (void)ReleaseLOSRegion(oldRegion);
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
    // P2-opt: Reuse pre-allocated member vectors (clear preserves capacity).
    compactMovableRegions_.clear();
    compactPinnedRegions_.clear();
    auto& movableRegions = compactMovableRegions_;
    auto& pinnedRegions = compactPinnedRegions_;
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

    // Phase 4: Reference update is handled via route-based forwarding.
    // CompactLOS builds the forwarding map; the WCollector's forward phase
    // resolves references through ghost region routes.

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
    // P2-opt: Reuse pre-allocated member vector.
    compactAllLiveRegions_.clear();
    auto& allLiveRegions = compactAllLiveRegions_;

    // Compacted regions (at new addresses)
    for (const auto& entry : compactPlan) {
        RegionInfo* newRegion = RegionInfo::GetRegionInfoAt(entry.newStart);
        allLiveRegions.push_back(newRegion);
        losOldLargeRegionList.PrependRegion(newRegion, RegionInfo::RegionType::LARGE_REGION);
    }

    // Movable regions that weren't in compactPlan (below pinned barrier, couldn't move).
    // P2-opt: Linear scan instead of unordered_set — compactPlan is typically < 20 entries,
    // so O(n*k) with small k is faster than hash table allocation + hashing.
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

    // Phase 7: Return RESERVE-borrowed space (with delayed return buffer).
    // After compaction, all live objects are packed toward losEnd.
    // losInactiveZone (= lowestLiveStart) includes pinned objects.
    // Safe return boundary: min(losInactiveZone, losStart).
    //   - losInactiveZone >= losStart → all objects in native LOS → return entire borrow.
    //   - losInactiveZone <  losStart → pinned object(s) remain in RESERVE → partial return.
    //
    // Return RESERVE-borrowed space fully up to the watermark.
    // No keep buffer — ShrinkInactiveZone and CompactLOS now run every GC,
    // so borrow-return ping-pong is mitigated by the fragmentation gate.
    size_t freedBytes = lowestLiveStart - oldInactiveZone;
    size_t shrunkBytes = 0;
    if (reserveManager != nullptr) {
        uintptr_t fullReturnWatermark = std::min(lowestLiveStart, losStart);
        shrunkBytes = reserveManager->ReclaimLOSBorrowed(fullReturnWatermark);
        DLOG(REPORT, "LOS RESERVE return: watermark → 0x%zx, returned %zu bytes",
             fullReturnWatermark, shrunkBytes);
    }

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
                // P0-opt: Release gap pages (madvise/mmap) and route to releasedTree
                // so TakeRegion avoids ClearUnits memset on the allocation hot path.
                RegionInfo* gapRegion = RegionInfo::InitRegionAt(gapStart, gapUnits,
                                                                 RegionInfo::UnitRole::LARGE_SIZED_UNITS);
                gapRegion->InitFreeUnits();
                size_t gapUnitIdx = gapRegion->GetUnitIdx();
                RegionInfo::ReleaseUnits(gapUnitIdx, gapUnits);
                losFreeMgr.AddReleaseUnits(static_cast<CartesianTree::Index>(gapUnitIdx),
                                           static_cast<CartesianTree::Count>(gapUnits));
                DLOG(REGION, "LOS rebuild freeMgr: gap [0x%zx, 0x%zx) = %zu units (released)",
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
            RegionInfo::ReleaseUnits(gapUnitIdx, gapUnits);
            losFreeMgr.AddReleaseUnits(static_cast<CartesianTree::Index>(gapUnitIdx),
                                       static_cast<CartesianTree::Count>(gapUnits));
            DLOG(REGION, "LOS rebuild freeMgr: trailing gap [0x%zx, 0x%zx) = %zu units (released)",
                     gapStart, losEnd, gapUnits);
        }
    }

    DLOG(REPORT, "LOS rebuild freeMgr: dirty=%u, released=%u",
         losFreeMgr.GetDirtyUnitCount(), losFreeMgr.GetReleasedUnitCount());
}

} // namespace MapleRuntime
