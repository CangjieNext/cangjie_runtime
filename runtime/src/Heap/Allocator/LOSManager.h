// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_LOS_MANAGER_H
#define MRT_LOS_MANAGER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "FreeRegionManager.h"
#include "RegionInfo.h"
#include "RegionList.h"
#include "Base/LogFile.h"

namespace MapleRuntime {

class RegionManager;
class ReserveManager;

/**
 * LOSManager - Large Object Space Manager
 *
 * Manages a dedicated space for large objects (> 32KB).
 * Uses its own FreeRegionManager instance (with independent dirtyTree + releaseTree),
 * separate from RegionManager's FreeRegionManager for MAIN space.
 *
 * Memory layout:
 *   [MAIN Space][RESERVE Zone][LOS Space]
 *   LOS grows from high address toward low address.
 *
 * Index convention:
 *   - FreeRegionManager internally uses local indices [0, losUnitCount).
 *   - baseUnit offset (= losUnitStart) is applied automatically when accessing RegionInfo.
 *   - External callers (RegionManager, WCollector) use global unit indices.
 */
class LOSManager {
public:
    using UnitIndex = CartesianTree::Index;
    using UnitCount = CartesianTree::Count;

    /**
     * @param manager       The parent RegionManager (needed by FreeRegionManager)
     * @param baseUnitIndex Global unit index of the first LOS unit (for FreeRegionManager offset)
     * @param reserveMgr    The RESERVE zone manager (may be nullptr)
     */
    LOSManager(RegionManager& manager, UnitCount baseUnitIndex = 0, ReserveManager* reserveMgr = nullptr)
        : losFreeMgr(manager, baseUnitIndex),
          reserveManager(reserveMgr),
          losOldLargeRegionList("LOS old large regions"),
          losRecentLargeRegionList("LOS recent large regions"),
          losLargeTraceRegions("LOS large trace regions")
    {}

    ~LOSManager() = default;

    LOSManager(const LOSManager&) = delete;
    LOSManager& operator=(const LOSManager&) = delete;

    /**
     * Initialize LOS space with given boundaries.
     * Creates the FreeRegionManager with baseUnit = unitStart so that
     * local tree indices map correctly to global RegionInfo indices.
     *
     * @param start     Start address of LOS space (low address boundary)
     * @param end       End address of LOS space (high address boundary)
     * @param unitStart Global unit index for LOS start
     * @param unitEnd   Global unit index for LOS end
     */
    void Initialize(uintptr_t start, uintptr_t end, UnitCount unitStart, UnitCount unitEnd);

    // ===== M2: Large object allocation in LOS =====
    // ===== M3: LOS GC sweep + MAIN→LOS migration =====

    /**
     * M3: Assemble LOS garbage candidates before GC mark phase.
     * Merges recent → old list and clears live info (analogous to RegionManager::AssembleLargeGarbageCandidates).
     */
    void AssembleLOSGarbageCandidates();

    /**
     * M3: Collect LOS garbage after mark phase.
     * Sweeps oldLargeRegionList — dead regions go to losFreeMgr (not MAIN's freeRegionManager).
     * Surviving regions get their mark bits reset.
     * @return Total garbage bytes reclaimed
     */
    size_t CollectLOSGarbage();

    /**
     * M3: Handle LOS trace regions after tracing completes.
     * Deactivates losLargeTraceRegions cache and merges into losRecentLargeRegionList.
     */
    void HandleLOSTraceRegions();

    /**
     * M3: Migrate surviving large objects from MAIN space into LOS.
     * Called during PostTrace (STW) after CollectLargeGarbage.
     *
     * For each surviving region in MAIN's oldLargeRegionList:
     *   - Skip if pinned (rawPointerCount > 0)
     *   - Try to allocate equivalent space in LOS
     *   - Copy object data
     *   - Record old→new address mapping for reference update
     *   - Reclaim old MAIN region
     *
     * @param mainOldList  MAIN's oldLargeRegionList (will be modified: migrated regions removed)
     * @param mainGarbageList  MAIN's garbageRegionList (migrated MAIN regions go here for reclaim)
     * @return Number of objects migrated
     */
    size_t MigrateLargeObjectsFromMain(RegionList& mainOldList, RegionList& mainGarbageList);

    /**
     * M3: Update all references across the heap to reflect migrated objects.
     * Scans roots and all heap objects, replacing old addresses with new ones.
     * Must be called during STW.
     */
    void UpdateMigratedReferences();

    /**
     * M3: Clear the migration forwarding table (called after migration is complete).
     */
    void ClearMigrationState() { migrationMap.clear(); }

    /**
     * M3: Check if there are pending migrations to process.
     */
    bool HasPendingMigrations() const { return !migrationMap.empty(); }

    // ===== M4a: LOS Sliding Compaction =====

    /**
     * M4a: Check if LOS compaction is needed.
     * Triggers when:
     *   - Fragmentation ratio > threshold (free units / active units > 30%)
     *   - LOS allocation failures occurred since last compaction
     * @return true if compaction should be performed
     */
    bool NeedsCompaction() const;

    /**
     * M4a: Perform sliding compaction on LOS space.
     * Objects slide toward losEnd (high address). Pinned objects (rawPointerCount > 0)
     * act as barriers — only compact above highest pinned object (Strategy A).
     *
     * Phases:
     *   1. Collect: separate movable vs pinned regions from both region lists
     *   2. Compute: calculate target addresses from losEnd toward low address
     *   3. Reference update: build forwarding map, scan heap + roots
     *   4. Move: memmove_s objects to new locations (high-to-low order)
     *   5. Metadata: rebuild region lists, rebuild losFreeMgr, update losInactiveZone
     *   6. Shrink: return freed low-address space to RESERVE
     *
     * Must be called during STW. Populates migrationMap for merged ref update with M3.
     * @return bytes freed by compaction
     */
    size_t CompactLOS();

    /**
     * M4a: Get current LOS fragmentation ratio.
     * = (free units in losFreeMgr) / (total active units)
     */
    double GetFragmentationRatio() const;

    /**
     * M4a: Compaction statistics
     */
    size_t GetCompactCount() const { return losCompactCount; }
    size_t GetCompactMovedBytes() const { return losCompactMovedBytes; }
    size_t GetCompactFreedBytes() const { return losCompactFreedBytes; }
    size_t GetCompactShrunkBytes() const { return losCompactShrunkBytes; }

    // Fragmentation threshold: compact when free units / active units > this ratio
    static constexpr double LOS_COMPACTION_FRAG_THRESHOLD = 0.3;

    // M5: Hysteresis — only return borrowed RESERVE space if LOS free ratio > this
    static constexpr double LOS_RESERVE_RETURN_THRESHOLD = 0.4;

    // M4a-async: Compaction coordination accessors
    bool IsCompacting() const { return isCompacting.load(std::memory_order_acquire); }
    void SetCompacting(bool v) { isCompacting.store(v, std::memory_order_release); }
    bool IsCompactionRequested() const { return compactionRequested.load(std::memory_order_acquire); }
    void SetCompactionRequested(bool v) { compactionRequested.store(v, std::memory_order_release); }

    /**
     * M3 statistics
     */
    size_t GetGarbageCollectedBytes() const { return losGarbageCollectedBytes; }
    size_t GetMigrateInCount() const { return losMigrateInCount; }
    size_t GetMigrateInBytes() const { return losMigrateInBytes; }

    /**
     * Allocate a large object in LOS space.
     *
     * Allocation flow (steps 1-2 in M2 plan):
     *   1. Try losFreeMgr (dirtyTree → releaseTree) for recycled regions
     *   2. Expand losInactiveZone toward low address for fresh regions
     *
     * Steps 3+ (RESERVE borrowing, compaction) are TODO stubs for later milestones.
     *
     * @param size  Allocation size in bytes (must be >= largeObjectThreshold)
     * @return      Allocated address, or 0 on failure (caller should fall back to MAIN)
     */
    uintptr_t AllocLarge(size_t size);

    /**
     * Enlist a newly allocated LOS large region into the appropriate region list.
     * Handles GC trace-phase awareness (analogous to RegionManager::AllocLarge region list logic).
     *
     * @param region  The newly allocated region
     */
    void EnlistLargeRegion(RegionInfo* region);

    /**
     * Get the LOS trace-region cache (for GC phase management)
     */
    RegionCache& GetLargeTraceRegions() { return losLargeTraceRegions; }

    /**
     * Get LOS space boundaries
     */
    uintptr_t GetLOSStart() const { return losStart; }
    uintptr_t GetLOSEnd() const { return losEnd; }
    uintptr_t GetInactiveZone() const { return losInactiveZone.load(std::memory_order_relaxed); }

    /**
     * Check if address is within LOS space
     */
    bool IsInLOS(uintptr_t addr) const { return addr >= losStart && addr < losEnd; }

    /**
     * Get LOS capacity and usage statistics
     */
    size_t GetCapacity() const { return losEnd - losStart; }
    size_t GetUsedSize() const { return losEnd - losInactiveZone.load(std::memory_order_relaxed); }
    size_t GetFreeSize() const { return losInactiveZone.load(std::memory_order_relaxed) - losStart; }

    /**
     * Statistics
     */
    size_t GetAllocCount() const { return losAllocCount; }
    size_t GetAllocFailCount() const { return losAllocFailCount; }
    size_t GetFallbackCount() const { return losFallbackCount; }
    size_t GetReserveBorrowCount() const { return losReserveBorrowCount; }

    // Increment statistics (called by RegionManager when fallback occurs)
    void IncrementAllocCount() { losAllocCount++; }
    void IncrementAllocFailCount() { losAllocFailCount++; }
    void IncrementFallbackCount() { losFallbackCount++; }

    /**
     * M5: Return RESERVE-borrowed free LOS regions to RESERVE.
     * During GC, dead RESERVE-borrowed regions are collected into pendingReserveReturns
     * instead of losFreeMgr. After GC, this method processes them:
     *   - Sort by address descending (return from watermark edge first)
     *   - Try ReturnFromLOS for each; on failure, add to losFreeMgr for reuse
     * Called from WCollector::DoGarbageCollection after CompactLOS.
     * @return Bytes returned to RESERVE
     */
    size_t ReturnBorrowedToReserve();

    /**
     * M5: Check if an address is in RESERVE-borrowed territory (below losStart).
     */
    bool IsInReserveTerritory(uintptr_t addr) const { return addr < losStart; }

    /**
     * Get region lists for GC
     */
    RegionList& GetOldLargeRegionList() { return losOldLargeRegionList; }
    RegionList& GetRecentLargeRegionList() { return losRecentLargeRegionList; }

    /**
     * Get FreeRegionManager for LOS space.
     * M2+ will use this for allocation and reclamation.
     */
    FreeRegionManager& GetFreeRegionManager() { return losFreeMgr; }

    /**
     * Get unit index range (global indices)
     */
    UnitCount GetUnitStart() const { return losUnitStart; }
    UnitCount GetUnitEnd() const { return losUnitEnd; }

private:
    /**
     * Try to allocate from LOS inactive zone (expand toward low address).
     * @param unitCount Number of units needed
     * @return Allocated RegionInfo, or nullptr on failure
     */
    RegionInfo* AllocFromInactiveZone(size_t unitCount);

    // Independent FreeRegionManager for LOS space (baseUnit = losUnitStart).
    // Has its own dirtyTree + releaseTree, separate from MAIN's FreeRegionManager.
    // When expanding/shrinking LOS, this manager's capacity should be adjusted accordingly.
    FreeRegionManager losFreeMgr;

    // Reserve manager for borrowing space (may be nullptr if RESERVE ratio is 0)
    ReserveManager* reserveManager;

    // LOS space boundaries
    uintptr_t losStart = 0;   // Low address boundary
    uintptr_t losEnd = 0;     // High address boundary

    // LOS inactive zone - allocation grows from losEnd toward losStart
    std::atomic<uintptr_t> losInactiveZone{0};

    // Unit index range for LOS (global indices)
    UnitCount losUnitStart = 0;
    UnitCount losUnitEnd = 0;

    // Region lists for LOS large objects
    RegionList losOldLargeRegionList;
    RegionList losRecentLargeRegionList;
    RegionCache losLargeTraceRegions;  // trace-phase region cache (analogous to RegionManager::largeTraceRegions)

    // M3: Reclaim a dead LOS region — return units to losFreeMgr
    // (NOT to MAIN's freeRegionManager). Mirrors RegionManager::CollectRegion/ReleaseRegion.
    size_t ReclaimLOSRegion(RegionInfo* region);
    size_t ReleaseLOSRegion(RegionInfo* region);

    // Statistics
    size_t losAllocCount = 0;
    size_t losAllocFailCount = 0;
    size_t losFallbackCount = 0;
    size_t losReserveBorrowCount = 0;    // number of RESERVE-borrowed LOS allocations

    // M3: GC statistics
    size_t losGarbageCollectedBytes = 0; // bytes reclaimed in last GC
    size_t losMigrateInCount = 0;        // objects migrated from MAIN to LOS
    size_t losMigrateInBytes = 0;        // bytes migrated from MAIN to LOS

    // M3/M4a: Migration forwarding map (old address → new address)
    // Used by both M3 migration and M4a compaction during STW reference update.
    // When both compaction and migration happen in the same GC cycle,
    // compaction entries are added first, then migration entries are appended.
    // Cleared after all reference updates complete.
    std::vector<std::pair<uintptr_t, uintptr_t>> migrationMap;

    // M4a: Compaction statistics
    size_t losCompactCount = 0;          // number of compactions performed
    size_t losCompactMovedBytes = 0;     // bytes moved in last compaction
    size_t losCompactFreedBytes = 0;     // bytes freed in last compaction
    size_t losCompactShrunkBytes = 0;    // bytes returned to RESERVE in last compaction
    size_t lastCompactAllocFailCount = 0; // losAllocFailCount at last compaction

    // M4a-async: Compaction coordination flags
    // isCompacting: true while compaction is in progress (sync or async).
    //   When true, AllocLarge() returns 0 immediately → forces MAIN fallback.
    std::atomic<bool> isCompacting{false};
    // compactionRequested: set by allocation path when LOS alloc fails and MAIN succeeds.
    //   Checked by FinalizerProcessor (async) and CompactLOSIfNeeded (sync).
    std::atomic<bool> compactionRequested{false};

    // M4a: Rebuild losFreeMgr trees after compaction, inserting gaps between live regions
    void RebuildFreeRegionManager(const std::vector<RegionInfo*>& allLiveRegions);

    // M4a: Return freed low-address space to RESERVE after compaction
    size_t ShrinkToReserve(size_t freedBytes);

    // M5: Pending RESERVE returns — RESERVE-borrowed dead regions held until explicit ReturnBorrowedToReserve()
    // <unitIdx, unitCount> pairs collected during CollectLOSGarbage
    std::vector<std::pair<size_t, size_t>> pendingReserveReturns;
};

} // namespace MapleRuntime

#endif // MRT_LOS_MANAGER_H
