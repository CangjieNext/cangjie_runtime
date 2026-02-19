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

#include "CartesianTree.h"
#include "RegionInfo.h"
#include "RegionList.h"

namespace MapleRuntime {

class ReserveManager;

/**
 * LOSManager - Large Object Space Manager
 *
 * Manages a dedicated space for large objects (> 32KB).
 * Has its own FreeRegionManager (dirtyTree + releaseTree) independent from RegionManager.
 *
 * Memory layout:
 *   [MAIN Space][RESERVE Zone][LOS Space]
 *   LOS grows from high address toward low address.
 */
class LOSManager {
public:
    using UnitIndex = CartesianTree::Index;
    using UnitCount = CartesianTree::Count;

    explicit LOSManager(ReserveManager* reserveMgr = nullptr)
        : reserveManager(reserveMgr),
          losOldLargeRegionList("LOS old large regions"),
          losRecentLargeRegionList("LOS recent large regions")
    {}

    ~LOSManager()
    {
        dirtyUnitTree.Fini();
        releasedUnitTree.Fini();
    }

    LOSManager(const LOSManager&) = delete;
    LOSManager& operator=(const LOSManager&) = delete;

    /**
     * Initialize LOS space with given boundaries.
     * @param start Start address of LOS space (low address boundary)
     * @param end End address of LOS space (high address boundary)
     * @param unitStart Global unit index for LOS start
     * @param unitEnd Global unit index for LOS end
     */
    void Initialize(uintptr_t start, uintptr_t end, UnitCount unitStart, UnitCount unitEnd);

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

    // Increment statistics (called by RegionManager when fallback occurs)
    void IncrementAllocFailCount() { losAllocFailCount++; }
    void IncrementFallbackCount() { losFallbackCount++; }

    /**
     * Get region lists for GC
     */
    RegionList& GetOldLargeRegionList() { return losOldLargeRegionList; }
    RegionList& GetRecentLargeRegionList() { return losRecentLargeRegionList; }

private:
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

    // Independent FreeRegionManager components for LOS
    std::mutex releasedUnitTreeMutex;
    CartesianTree releasedUnitTree;

    std::mutex dirtyUnitTreeMutex;
    CartesianTree dirtyUnitTree;

    // Region lists for LOS large objects
    RegionList losOldLargeRegionList;
    RegionList losRecentLargeRegionList;

    // Statistics
    size_t losAllocCount = 0;
    size_t losAllocFailCount = 0;
    size_t losFallbackCount = 0;
};

} // namespace MapleRuntime

#endif // MRT_LOS_MANAGER_H
