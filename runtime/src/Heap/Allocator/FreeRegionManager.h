// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_FREE_REGION_MANAGER_H
#define MRT_FREE_REGION_MANAGER_H

#include "CartesianTree.h"
#include "RegionInfo.h"
#include "Common/ScopedObjectAccess.h"

namespace MapleRuntime {
class RegionManager;

// This class is and should be accessed only for region allocation. we do not rely on it to check region status.
class FreeRegionManager {
    using UnitIndex = CartesianTree::Index;
    using UnitCount = CartesianTree::Count;

public:
    // @param baseUnitIndex: global unit index of the first unit managed by this instance.
    //   For MAIN space, baseUnitIndex = 0. For LOS space, baseUnitIndex = losUnitStart.
    explicit FreeRegionManager(RegionManager& manager, UnitIndex baseUnitIndex = 0)
        : regionManager(manager), baseUnit(baseUnitIndex) {}

    virtual ~FreeRegionManager()
    {
        dirtyUnitTree.Fini();
        releasedUnitTree.Fini();
    }

    void Initialize(UnitCount regionCnt)
    {
        releasedUnitTree.Init(regionCnt, baseUnit);
        dirtyUnitTree.Init(regionCnt, baseUnit);
    }

    UnitIndex GetBaseUnit() const { return baseUnit; }

    // Enable low-address-first allocation (for MAIN space defragmentation).
    // When enabled, TakeRegion prefers the lowest-address free chunk,
    // packing live regions toward regionHeapStart so that free space
    // consolidates near inactiveZone for return to RESERVE.
    void SetLowAddressFirst(bool enabled) { lowAddressFirst = enabled; }

    RegionInfo* TakeRegion(size_t num, RegionInfo::UnitRole uclass, bool expectPhysicalMem)
    {
        UnitIndex idx = 0;
        bool tryDirtyTree = true;
        bool tryReleasedTree = true;

        // try as hard as we can to take free regions for allocation.
        while (tryDirtyTree || tryReleasedTree) {
            // first try to get a dirty region.
            if (tryDirtyTree && dirtyUnitTreeMutex.try_lock()) {
                bool dirtyOk = lowAddressFirst ? dirtyUnitTree.TakeUnitsLowAddr(num, idx)
                                               : dirtyUnitTree.TakeUnits(num, idx);
                if (dirtyOk) {
                    UnitIndex globalIdx = idx + baseUnit;
                    DLOG(REGION, "c-tree %p alloc dirty units[%u+%u, %u) @[0x%zx, 0x%zx), %u dirty-units left",
                        &dirtyUnitTree, globalIdx, num, globalIdx + num,
                        RegionInfo::GetUnitAddress(globalIdx),
                        RegionInfo::GetUnitAddress(globalIdx + num), dirtyUnitTree.GetTotalCount());

                    // it makes sense to slow down allocation by clearing region memory.
                    RegionInfo::ClearUnits(globalIdx, num);
                    RegionInfo* region = RegionInfo::InitRegion(globalIdx, num, uclass);
                    dirtyUnitTreeMutex.unlock();
                    return region;
                }
                tryDirtyTree = false; // once we fail to take units, stop trying.
                dirtyUnitTreeMutex.unlock();
            }

            // then try to get a released region.
            if (tryReleasedTree && releasedUnitTreeMutex.try_lock()) {
                bool releasedOk = lowAddressFirst ? releasedUnitTree.TakeUnitsLowAddr(num, idx)
                                                  : releasedUnitTree.TakeUnits(num, idx);
                if (releasedOk) {
                    UnitIndex globalIdx = idx + baseUnit;
#ifdef _WIN64
                    MemMap::CommitMemory(
                        reinterpret_cast<void*>(RegionInfo::GetUnitAddress(globalIdx)),
                        num * RegionInfo::UNIT_SIZE);
#endif
                    DLOG(REGION, "c-tree %p alloc released units[%u+%u, %u) @[0x%zx, 0x%zx), %u released-units left",
                        &releasedUnitTree, globalIdx, num, globalIdx + num,
                        RegionInfo::GetUnitAddress(globalIdx),
                        RegionInfo::GetUnitAddress(globalIdx + num), releasedUnitTree.GetTotalCount());
                    RegionInfo* region = RegionInfo::InitRegion(globalIdx, num, uclass);
                    releasedUnitTreeMutex.unlock();
                    PrehandleReleasedUnit(expectPhysicalMem, globalIdx, num);
                    return region;
                }
                tryReleasedTree = false; // once we fail to take units, stop trying.
                releasedUnitTreeMutex.unlock();
            }
            ScopedEnterSaferegion enterSaferegion(true);
        }

        return nullptr;
    }

    // Take a region from the lowest available address, with allocation end below upperBound (global unit index).
    // Used by MAIN space in the unified FreeRegionManager to respect inactiveZone boundary.
    RegionInfo* TakeRegionBounded(size_t num, RegionInfo::UnitRole uclass, bool expectPhysicalMem,
                                  UnitIndex upperBound)
    {
        UnitIndex localUpperBound = upperBound - baseUnit;
        UnitIndex idx = 0;
        bool tryDirtyTree = true;
        bool tryReleasedTree = true;

        while (tryDirtyTree || tryReleasedTree) {
            if (tryDirtyTree && dirtyUnitTreeMutex.try_lock()) {
                bool dirtyOk = dirtyUnitTree.TakeUnitsLowAddrBounded(num, idx, localUpperBound);
                if (dirtyOk) {
                    UnitIndex globalIdx = idx + baseUnit;
                    DLOG(REGION, "c-tree %p alloc dirty units (bounded)[%u+%zu, %u) @[0x%zx, 0x%zx)",
                        &dirtyUnitTree, globalIdx, num, static_cast<UnitIndex>(globalIdx + num),
                        RegionInfo::GetUnitAddress(globalIdx),
                        RegionInfo::GetUnitAddress(globalIdx + num));
                    RegionInfo::ClearUnits(globalIdx, num);
                    RegionInfo* region = RegionInfo::InitRegion(globalIdx, num, uclass);
                    dirtyUnitTreeMutex.unlock();
                    return region;
                }
                tryDirtyTree = false;
                dirtyUnitTreeMutex.unlock();
            }

            if (tryReleasedTree && releasedUnitTreeMutex.try_lock()) {
                bool releasedOk = releasedUnitTree.TakeUnitsLowAddrBounded(num, idx, localUpperBound);
                if (releasedOk) {
                    UnitIndex globalIdx = idx + baseUnit;
#ifdef _WIN64
                    MemMap::CommitMemory(
                        reinterpret_cast<void*>(RegionInfo::GetUnitAddress(globalIdx)),
                        num * RegionInfo::UNIT_SIZE);
#endif
                    DLOG(REGION, "c-tree %p alloc released units (bounded)[%u+%zu, %u) @[0x%zx, 0x%zx)",
                        &releasedUnitTree, globalIdx, num, static_cast<UnitIndex>(globalIdx + num),
                        RegionInfo::GetUnitAddress(globalIdx),
                        RegionInfo::GetUnitAddress(globalIdx + num));
                    RegionInfo* region = RegionInfo::InitRegion(globalIdx, num, uclass);
                    releasedUnitTreeMutex.unlock();
                    PrehandleReleasedUnit(expectPhysicalMem, globalIdx, num);
                    return region;
                }
                tryReleasedTree = false;
                releasedUnitTreeMutex.unlock();
            }
            ScopedEnterSaferegion enterSaferegion(true);
        }
        return nullptr;
    }

    // Take a region from the highest available address, with high-end allocation start >= lowerBound.
    // Allocates from the HIGH end of the chosen block.
    // Used by LOS space in the unified FreeRegionManager to respect losInactiveZone boundary.
    RegionInfo* TakeRegionHighAddrBounded(size_t num, RegionInfo::UnitRole uclass, bool expectPhysicalMem,
                                          UnitIndex lowerBound)
    {
        UnitIndex localLowerBound = lowerBound - baseUnit;
        UnitIndex idx = 0;
        bool tryDirtyTree = true;
        bool tryReleasedTree = true;

        while (tryDirtyTree || tryReleasedTree) {
            if (tryDirtyTree && dirtyUnitTreeMutex.try_lock()) {
                bool dirtyOk = dirtyUnitTree.TakeUnitsHighAddrBounded(num, idx, localLowerBound);
                if (dirtyOk) {
                    UnitIndex globalIdx = idx + baseUnit;
                    DLOG(REGION, "c-tree %p alloc dirty units (high-bounded)[%u+%zu, %u) @[0x%zx, 0x%zx)",
                        &dirtyUnitTree, globalIdx, num, static_cast<UnitIndex>(globalIdx + num),
                        RegionInfo::GetUnitAddress(globalIdx),
                        RegionInfo::GetUnitAddress(globalIdx + num));
                    RegionInfo::ClearUnits(globalIdx, num);
                    RegionInfo* region = RegionInfo::InitRegion(globalIdx, num, uclass);
                    dirtyUnitTreeMutex.unlock();
                    return region;
                }
                tryDirtyTree = false;
                dirtyUnitTreeMutex.unlock();
            }

            if (tryReleasedTree && releasedUnitTreeMutex.try_lock()) {
                bool releasedOk = releasedUnitTree.TakeUnitsHighAddrBounded(num, idx, localLowerBound);
                if (releasedOk) {
                    UnitIndex globalIdx = idx + baseUnit;
#ifdef _WIN64
                    MemMap::CommitMemory(
                        reinterpret_cast<void*>(RegionInfo::GetUnitAddress(globalIdx)),
                        num * RegionInfo::UNIT_SIZE);
#endif
                    DLOG(REGION, "c-tree %p alloc released units (high-bounded)[%u+%zu, %u) @[0x%zx, 0x%zx)",
                        &releasedUnitTree, globalIdx, num, static_cast<UnitIndex>(globalIdx + num),
                        RegionInfo::GetUnitAddress(globalIdx),
                        RegionInfo::GetUnitAddress(globalIdx + num));
                    RegionInfo* region = RegionInfo::InitRegion(globalIdx, num, uclass);
                    releasedUnitTreeMutex.unlock();
                    PrehandleReleasedUnit(expectPhysicalMem, globalIdx, num);
                    return region;
                }
                tryReleasedTree = false;
                releasedUnitTreeMutex.unlock();
            }
            ScopedEnterSaferegion enterSaferegion(true);
        }
        return nullptr;
    }

    // Check if a high-address bounded allocation of 'num' units would succeed.
    // Used by LOSManager::CanAllocLargeAfterGC to check if compaction is needed.
    bool HasSuitableRegionHighAddr(size_t num, UnitIndex lowerBound) const
    {
        UnitIndex localLower = lowerBound - baseUnit;
        return dirtyUnitTree.HasSuitableHighAddr(num, localLower) ||
               releasedUnitTree.HasSuitableHighAddr(num, localLower);
    }

    // Remove all free blocks in [globalLo, globalHi) from both trees.
    // Used by CompactLOS to clear LOS-range blocks before rebuilding.
    UnitCount RemoveInRange(UnitIndex globalLo, UnitIndex globalHi)
    {
        UnitIndex localLo = globalLo - baseUnit;
        UnitIndex localHi = globalHi - baseUnit;
        UnitCount removed = 0;
        {
            std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
            removed += dirtyUnitTree.RemoveInRange(localLo, localHi);
        }
        {
            std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
            removed += releasedUnitTree.RemoveInRange(localLo, localHi);
        }
        return removed;
    }

    // add units [idx, idx + num), idx is global unit index
    void AddGarbageUnits(UnitIndex idx, UnitCount num)
    {
        UnitIndex localIdx = idx - baseUnit;
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        if (UNLIKELY(!dirtyUnitTree.MergeInsert(localIdx, num, true))) {
            bool allFree = true;
            for (UnitCount i = 0; i < num; ++i) {
                RegionInfo* unitInfo = RegionInfo::GetRegionInfo(idx + i);
                if (unitInfo == nullptr || !unitInfo->IsFreeRegion()) {
                    allFree = false;
                    break;
                }
            }
            if (allFree) {
                DLOG(REGION, "skip duplicate dirty units insertion [%u+%u, %u)", idx, num, idx + num);
                return;
            }
            LOG(RTLOG_FATAL, "tid %d: failed to add dirty units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }

    void AddReleaseUnits(UnitIndex idx, UnitCount num)
    {
        UnitIndex localIdx = idx - baseUnit;
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        if (UNLIKELY(!releasedUnitTree.MergeInsert(localIdx, num, true))) {
            bool allFree = true;
            for (UnitCount i = 0; i < num; ++i) {
                RegionInfo* unitInfo = RegionInfo::GetRegionInfo(idx + i);
                if (unitInfo == nullptr || !unitInfo->IsFreeRegion()) {
                    allFree = false;
                    break;
                }
            }
            if (allFree) {
                DLOG(REGION, "skip duplicate release units insertion [%u+%u, %u)", idx, num, idx + num);
                return;
            }
            LOG(RTLOG_FATAL, "tid %d: failed to add release units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }

    UnitCount GetDirtyUnitCount() const { return dirtyUnitTree.GetTotalCount(); }
    UnitCount GetReleasedUnitCount() const { return releasedUnitTree.GetTotalCount(); }

    // Largest contiguous free block (units) across both trees.
    UnitCount GetMaxContiguousFreeUnits() const
    {
        UnitCount dirtyMax = dirtyUnitTree.GetMaxContiguousCount();
        UnitCount releasedMax = releasedUnitTree.GetMaxContiguousCount();
        return dirtyMax > releasedMax ? dirtyMax : releasedMax;
    }

    // Total number of disjoint free chunks (fragments) across both trees.
    size_t GetFreeFragmentCount() const
    {
        return static_cast<size_t>(dirtyUnitTree.GetFragmentCount()) +
               static_cast<size_t>(releasedUnitTree.GetFragmentCount());
    }

    // Mark all free units in stateArray: dirty tree units → dirtyVal, released tree units → releasedVal.
    // Uses global unit indices (tree-local + baseUnit offset handled inside CartesianTree).
    void MarkFreeUnits(uint8_t* stateArray, size_t arraySize, uint8_t dirtyVal, uint8_t releasedVal) const
    {
        dirtyUnitTree.MarkFreeBlockUnits(stateArray, arraySize, dirtyVal);
        releasedUnitTree.MarkFreeBlockUnits(stateArray, arraySize, releasedVal);
    }

    // Clear both trees (used by LOS compaction to rebuild free region tracking)
    void ClearTrees()
    {
        {
            std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
            dirtyUnitTree.ClearTree();
        }
        {
            std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
            releasedUnitTree.ClearTree();
        }
    }

    // Remove all free blocks at or above the given global unit index threshold.
    // Blocks straddling the threshold are truncated (only the portion below kept).
    // Returns the total unit count removed from both trees.
    // Called under STW — mutex acquired for safety but no real contention.
    UnitCount RemoveAboveIndex(UnitIndex globalThreshold)
    {
        UnitIndex localThreshold = globalThreshold - baseUnit;
        UnitCount removed = 0;
        {
            std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
            removed += dirtyUnitTree.RemoveAboveIndex(localThreshold);
        }
        {
            std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
            removed += releasedUnitTree.RemoveAboveIndex(localThreshold);
        }
        return removed;
    }

#if defined(MRT_DEBUG)
    void DumpReleasedUnitTree() const { releasedUnitTree.DumpTree("released-unit tree"); }
    void DumpDirtyUnitTree() const { dirtyUnitTree.DumpTree("dirty-unit tree"); }
#endif

    size_t CalculateBytesToRelease() const;
    size_t ReleaseGarbageRegions(size_t targetCachedSize);

private:
    inline void PrehandleReleasedUnit(bool expectPhysicalMem, size_t idx, size_t num) const
    {
        if (expectPhysicalMem) {
            RegionInfo::ClearUnits(idx, num);
        }
    }
    RegionManager& regionManager;
    UnitIndex baseUnit = 0; // global unit index offset (0 for MAIN, losUnitStart for LOS)
    bool lowAddressFirst = false; // prefer lowest-address free chunk (MAIN defrag)

    // physical pages of released units are probably released and they are prepared for allocation.
    std::mutex releasedUnitTreeMutex;
    CartesianTree releasedUnitTree;

    // dirty units are neither cleared nor released, thus must be zeroed explicitly for allocation.
    std::mutex dirtyUnitTreeMutex;
    CartesianTree dirtyUnitTree;
};
} // namespace MapleRuntime
#endif // MRT_FREE_REGION_MANAGER_H
