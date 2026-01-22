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
    explicit FreeRegionManager(RegionManager& manager) : regionManager(manager) {}

    virtual ~FreeRegionManager()
    {
        dirtyUnitTree.Fini();
        releasedUnitTree.Fini();
    }

    void Initialize(UnitCount regionCnt)
    {
        releasedUnitTree.Init(regionCnt);
        dirtyUnitTree.Init(regionCnt);
    }

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
                    DLOG(REGION, "c-tree %p alloc dirty units[%u+%u, %u) @[0x%zx, 0x%zx), %u dirty-units left",
                        &dirtyUnitTree, idx, num, idx + num, RegionInfo::GetUnitAddress(idx),
                        RegionInfo::GetUnitAddress(idx + num), dirtyUnitTree.GetTotalCount());

                    // it makes sense to slow down allocation by clearing region memory.
                    RegionInfo::ClearUnits(idx, num);
                    RegionInfo* region = RegionInfo::InitRegion(idx, num, uclass);
                    dirtyUnitTreeMutex.unlock();
                    return region;
                }
                tryDirtyTree = false; // once we fail to take units, stop trying.
                dirtyUnitTreeMutex.unlock();
            }

            // then try to get a released region.
            if (tryReleasedTree && releasedUnitTreeMutex.try_lock()) {
                if (releasedUnitTree.TakeUnits(num, idx)) {
#ifdef _WIN64
                    MemMap::CommitMemory(
                        reinterpret_cast<void*>(RegionInfo::GetUnitAddress(idx)), num * RegionInfo::UNIT_SIZE);
#endif
                    DLOG(REGION, "c-tree %p alloc released units[%u+%u, %u) @[0x%zx, 0x%zx), %u released-units left",
                        &releasedUnitTree, idx, num, idx + num, RegionInfo::GetUnitAddress(idx),
                        RegionInfo::GetUnitAddress(idx + num), releasedUnitTree.GetTotalCount());
                    RegionInfo* region = RegionInfo::InitRegion(idx, num, uclass);
                    releasedUnitTreeMutex.unlock();
                    PrehandleReleasedUnit(expectPhysicalMem, idx, num);
                    return region;
                }
                tryReleasedTree = false; // once we fail to take units, stop trying.
                releasedUnitTreeMutex.unlock();
            }
            ScopedEnterSaferegion enterSaferegion(true);
        }

        return nullptr;
    }

    // add units [idx, idx + num)
    void AddGarbageUnits(UnitIndex idx, UnitCount num)
    {
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        if (UNLIKELY(!dirtyUnitTree.MergeInsert(idx, num, true))) {
            LOG(RTLOG_FATAL, "tid %d: failed to add dirty units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }

    void AddReleaseUnits(UnitIndex idx, UnitCount num)
    {
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        if (UNLIKELY(!releasedUnitTree.MergeInsert(idx, num, true))) {
            LOG(RTLOG_FATAL, "tid %d: failed to add release units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }

    UnitCount GetDirtyUnitCount() const { return dirtyUnitTree.GetTotalCount(); }
    UnitCount GetReleasedUnitCount() const { return releasedUnitTree.GetTotalCount(); }

    // Collect size distribution information of nodes in dirtyUnitTree
    struct SizeDistribution {
        size_t countLess2 = 0; // Number of nodes with less than 2 units
        size_t count2To4 = 0; // Number of nodes with 2 or more but less than 4 units
        size_t count4To6 = 0; // Number of nodes with 4 or more but less than 6 units
        size_t count6To8 = 0; // Number of nodes with 6 or more but less than 8 units
        size_t count8To16 = 0; // Number of nodes with 8 or more but less than 16 units
        size_t count16To32 = 0; // Number of nodes with 16 or more but less than or equal to 32 units
        size_t count32To64 = 0; // Number of nodes with 32 or more but less than 64 units
        size_t count64To128 = 0; // Number of nodes with 64 or more but less than 128 units
        size_t count128To256 = 0; // Number of nodes with 128 or more but less than 256 units
        size_t count256To512 = 0; // Number of nodes with 256 or more but less than 512 units
        size_t count512To1024 = 0; // Number of nodes with 512 or more but less than 1024 units
        size_t count1024To2048 = 0; // Number of nodes with 1024 or more but less than 2048 units
        size_t count2048To2560 = 0; // Number of nodes with 2048 or more but less than or equal to 2560 units
        size_t countGreater2560 = 0; // Number of nodes with more than 2560 units
        size_t totalSizeLess2 = 0; // Total size of nodes with less than 2 units
        size_t totalSize2To4 = 0; // Total size of nodes with 2 or more but less than 4 units
        size_t totalSize4To6 = 0; // Total size of nodes with 4 or more but less than 6 units
        size_t totalSize6To8 = 0; // Total size of nodes with 6 or more but less than 8 units
        size_t totalSize8To16 = 0; // Total size of nodes with 8 or more but less than 16 units
        size_t totalSize16To32 = 0; // Total size of nodes with 16 or more but less than or equal to 32 units
        size_t totalSize32To64 = 0; // Total size of nodes with 32 or more but less than 64 units
        size_t totalSize64To128 = 0; // Total size of nodes with 64 or more but less than 128 units
        size_t totalSize128To256 = 0; // Total size of nodes with 128 or more but less than 256 units
        size_t totalSize256To512 = 0; // Total size of nodes with 256 or more but less than 512 units
        size_t totalSize512To1024 = 0; // Total size of nodes with 512 or more but less than 1024 units
        size_t totalSize1024To2048 = 0; // Total size of nodes with 1024 or more but less than 2048 units
        size_t totalSize2048To2560 = 0; // Total size of nodes with 2048 or more but less than or equal to 2560 units
        size_t totalSizeGreater2560 = 0; // Total size of nodes with more than 2560 units
    };
    SizeDistribution GetDirtyTreeSizeDistribution() const;
    SizeDistribution GetReleasedTreeSizeDistribution() const;

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

    // physical pages of released units are probably released and they are prepared for allocation.
    mutable std::mutex releasedUnitTreeMutex;
    CartesianTree releasedUnitTree;

    // dirty units are neither cleared nor released, thus must be zeroed explicitly for allocation.
    mutable std::mutex dirtyUnitTreeMutex;
    CartesianTree dirtyUnitTree;
};
} // namespace MapleRuntime
#endif // MRT_FREE_REGION_MANAGER_H
