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
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "FreeRegionManager.h"
#include "RegionInfo.h"
#include "RegionList.h"
#include "ReserveManager.h"
#include "Base/LogFile.h"

namespace MapleRuntime {

class RegionManager;

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
 *   - FreeRegionManager internally uses local indices [0, reserveUnitCount + losUnitCount).
 *   - baseUnit offset (= mainUnitCount) is applied automatically when accessing RegionInfo.
 *   - This covers both the RESERVE extension area and the native LOS space,
 *     allowing RESERVE-borrowed dead regions to be recycled through losFreeMgr normally.
 *   - External callers (RegionManager, WCollector) use global unit indices.
 */
class LOSManager {
public:
    using UnitIndex = CartesianTree::Index;
    using UnitCount = CartesianTree::Count;

    struct CompactEntry {
        uintptr_t oldStart = 0;
        uintptr_t newStart = 0;
        size_t regionSize = 0;
        size_t unitCount = 0;
    };

    /**
     * @param manager       The parent RegionManager (needed by FreeRegionManager)
     * @param baseUnitIndex Global unit index of first RESERVE unit (= mainUnitCount).
     *                      Covers RESERVE extension area + native LOS.
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
     * Creates the FreeRegionManager with baseUnit = mainUnitCount so that
     * local tree indices map correctly to global RegionInfo indices.
     * Capacity covers RESERVE extension area + native LOS space.
     *
     * @param start            Start address of LOS space (low address boundary)
     * @param end              End address of LOS space (high address boundary)
     * @param unitStart        Global unit index for LOS start
     * @param unitEnd          Global unit index for LOS end
     * @param reserveUnitCount Number of RESERVE units (for FreeRegionManager capacity)
     */
    void Initialize(uintptr_t start, uintptr_t end, UnitCount unitStart, UnitCount unitEnd,
                    UnitCount reserveUnitCount = 0);

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

    // Pending compaction plan produced in PostTrace and consumed in forward/finalize phase.
    bool HasPendingCompactionPlan() const
    {
        std::lock_guard<std::mutex> lock(compactionPlanMutex);
        return !pendingCompactionPlan.empty();
    }

    size_t GetPendingCompactionPlanSize() const
    {
        std::lock_guard<std::mutex> lock(compactionPlanMutex);
        return pendingCompactionPlan.size();
    }

    void SetPendingCompactionPlan(std::vector<CompactEntry> plan)
    {
        std::lock_guard<std::mutex> lock(compactionPlanMutex);
        pendingCompactionPlan = std::move(plan);
    }

    std::vector<CompactEntry> GetPendingCompactionPlanSnapshot() const
    {
        std::lock_guard<std::mutex> lock(compactionPlanMutex);
        return pendingCompactionPlan;
    }

    void ClearPendingCompactionPlan()
    {
        std::lock_guard<std::mutex> lock(compactionPlanMutex);
        pendingCompactionPlan.clear();
    }

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
     * A-step2: Build compaction plan only (no memmove / no tree rebuild).
     * Produces old->new region mapping for later execution stage.
     * @param outPlan Planned movable regions (old/new/size/unitCount)
     * @return true if there are movable regions to compact
     */
    bool BuildCompactionPlan(std::vector<CompactEntry>& outPlan);

    // Install pending compaction plan into forwarding metadata in one shot.
    // This stage does not rebuild region lists; finalization happens after forward convergence.
    bool InstallPendingCompactionForForwarding();

    // Ensure the compaction route for a LOS object address is installed.
    // Used by read-barrier assist path to push predecessor entries first.
    bool EnsureCompactionRouteForObject(uintptr_t objAddr);

    // Finalize pending compaction entries that have already been forwarded.
    // Unfinished entries are retained for the next GC convergence round.
    // Returns number of finalized entries.
    size_t FinalizePendingCompaction();

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

    // A-STEP1: Freeze LOS writes during compaction window.
    // When frozen, AllocLarge() fails fast and caller falls back to MAIN.
    bool IsCompactionFrozen() const { return losCompactionFreeze.load(std::memory_order_acquire); }
    void FreezeForCompaction() { losCompactionFreeze.store(true, std::memory_order_release); }
    void UnfreezeAfterCompaction() { losCompactionFreeze.store(false, std::memory_order_release); }

    /**
     * M3 statistics
     */
    size_t GetGarbageCollectedBytes() const { return losGarbageCollectedBytes; }

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
     * P4: Effective LOS lower boundary — includes RESERVE-borrowed runway.
     * When RESERVE borrowing is active, losWatermark < losStart and the
     * losInactiveZone can expand below losStart into the borrowed area.
     */
    uintptr_t GetEffectiveLOSStart() const
    {
        if (reserveManager != nullptr) {
            return reserveManager->GetLOSWatermark();
        }
        return losStart;
    }

    /**
     * Check if address is within LOS space
     */
    bool IsInLOS(uintptr_t addr) const { return addr >= losStart && addr < losEnd; }

    /**
     * Get LOS capacity and usage statistics
     */
    size_t GetCapacity() const { return losEnd - losStart; }
    size_t GetUsedSize() const { return losEnd - losInactiveZone.load(std::memory_order_relaxed); }
    size_t GetFreeSize() const {
        uintptr_t iz = losInactiveZone.load(std::memory_order_relaxed);
        uintptr_t effectiveStart = GetEffectiveLOSStart();
        return (iz > effectiveStart) ? (iz - effectiveStart) : 0;
    }

    /**
     * Statistics
     */
    size_t GetAllocCount() const { return losAllocCount.load(std::memory_order_relaxed); }
    size_t GetAllocFailCount() const { return losAllocFailCount.load(std::memory_order_relaxed); }
    size_t GetFallbackCount() const { return losFallbackCount.load(std::memory_order_relaxed); }
    size_t GetReserveBorrowCount() const { return losReserveBorrowCount.load(std::memory_order_relaxed); }

    // Increment statistics (called by RegionManager when fallback occurs)
    void IncrementAllocCount() { losAllocCount.fetch_add(1, std::memory_order_relaxed); }
    void IncrementAllocFailCount() { losAllocFailCount.fetch_add(1, std::memory_order_relaxed); }
    void IncrementFallbackCount() { losFallbackCount.fetch_add(1, std::memory_order_relaxed); }

    /**
     * M5: Check if an address is in RESERVE-borrowed territory (below losStart).
     */
    bool IsInReserveTerritory(uintptr_t addr) const { return addr < losStart; }

    /**
     * Get the compaction migration map (old address → new address).
     * Used by STW reference update after CompactLOS memmove.
     */
    const std::unordered_map<uintptr_t, uintptr_t>& GetMigrationMap() const { return migrationMap; }
    void ClearMigrationMap() { migrationMap.clear(); }

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
    const FreeRegionManager& GetFreeRegionManager() const { return losFreeMgr; }

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

    // Independent FreeRegionManager for LOS space (baseUnit = mainUnitCount).
    // Has its own dirtyTree + releaseTree, separate from MAIN's FreeRegionManager.
    // Capacity covers RESERVE extension area + native LOS space.
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

    // Statistics (atomic: incremented by concurrent mutator threads in AllocLarge)
    std::atomic<size_t> losAllocCount{0};
    std::atomic<size_t> losAllocFailCount{0};
    std::atomic<size_t> losFallbackCount{0};
    std::atomic<size_t> losReserveBorrowCount{0};    // number of RESERVE-borrowed LOS allocations

    // M3: GC statistics
    size_t losGarbageCollectedBytes = 0; // bytes reclaimed in last GC

    // M4a: Compaction forwarding map (old address → new address)
    // Used by compaction during STW reference update.
    // Cleared after all reference updates complete.
    std::unordered_map<uintptr_t, uintptr_t> migrationMap;

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

    // A-STEP1: true means LOS accepts reads/reclaims but rejects new large allocations.
    std::atomic<bool> losCompactionFreeze{false};

    // M4a: Rebuild losFreeMgr trees after compaction, inserting gaps between live regions
    void RebuildFreeRegionManager(const std::vector<RegionInfo*>& allLiveRegions);

    // (removed: ShrinkToReserve — replaced by direct ReclaimLOSBorrowed in CompactLOS Phase 7)

    // A-step2: saved compaction plan between PostTrace and forward/finalize stages.
    std::vector<CompactEntry> pendingCompactionPlan;
    mutable std::mutex compactionPlanMutex;

    static bool RangesOverlap(uintptr_t aStart, uintptr_t aEnd, uintptr_t bStart, uintptr_t bEnd)
    {
        return aStart < bEnd && bStart < aEnd;
    }

    bool IsCompactionEntryInstalledLocked(const CompactEntry& entry) const;
    bool IsCompactionEntryResolvedLocked(const CompactEntry& entry) const;
    bool InstallCompactionEntryLocked(const CompactEntry& entry);
    bool InstallCompactionEntryWithPredecessorsLocked(const CompactEntry& entry,
                                                      std::unordered_set<uintptr_t>& visiting);
};

} // namespace MapleRuntime

#endif // MRT_LOS_MANAGER_H
