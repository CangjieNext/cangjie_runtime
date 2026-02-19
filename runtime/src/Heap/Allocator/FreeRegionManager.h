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

    RegionInfo* TakeRegion(size_t num, RegionInfo::UnitRole uclass, bool expectPhysicalMem)
    {
        UnitIndex idx = 0;
        bool tryDirtyTree = true;
        bool tryReleasedTree = true;

        // try as hard as we can to take free regions for allocation.
        while (tryDirtyTree || tryReleasedTree) {
            // first try to get a dirty region.
            if (tryDirtyTree && dirtyUnitTreeMutex.try_lock()) {
                if (dirtyUnitTree.TakeUnits(num, idx)) {
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
                if (releasedUnitTree.TakeUnits(num, idx)) {
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

    // add units [idx, idx + num), idx is global unit index
    void AddGarbageUnits(UnitIndex idx, UnitCount num)
    {
        UnitIndex localIdx = idx - baseUnit;
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        if (UNLIKELY(!dirtyUnitTree.MergeInsert(localIdx, num, true))) {
            LOG(RTLOG_FATAL, "tid %d: failed to add dirty units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }

    void AddReleaseUnits(UnitIndex idx, UnitCount num)
    {
        UnitIndex localIdx = idx - baseUnit;
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        if (UNLIKELY(!releasedUnitTree.MergeInsert(localIdx, num, true))) {
            LOG(RTLOG_FATAL, "tid %d: failed to add release units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }

    UnitCount GetDirtyUnitCount() const { return dirtyUnitTree.GetTotalCount(); }
    UnitCount GetReleasedUnitCount() const { return releasedUnitTree.GetTotalCount(); }

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

    // physical pages of released units are probably released and they are prepared for allocation.
    std::mutex releasedUnitTreeMutex;
    CartesianTree releasedUnitTree;

    // dirty units are neither cleared nor released, thus must be zeroed explicitly for allocation.
    std::mutex dirtyUnitTreeMutex;
    CartesianTree dirtyUnitTree;
};
} // namespace MapleRuntime
#endif // MRT_FREE_REGION_MANAGER_H
