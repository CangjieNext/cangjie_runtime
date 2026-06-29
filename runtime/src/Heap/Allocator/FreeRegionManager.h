// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_FREE_REGION_MANAGER_H
#define MRT_FREE_REGION_MANAGER_H

#include <atomic>
#include <memory>
#include "CartesianTree.h"
#include "RegionInfo.h"
#include "Common/ScopedObjectAccess.h"

namespace MapleRuntime {
class RegionManager;

// Bitmap tracking which units are currently in dirtyUnitTree.
class DirtyBitmap {
public:
    void Init(size_t unitCount)
    {
        totalUnits = unitCount;
        size_t words = (unitCount + BITS_PER_WORD - 1) / BITS_PER_WORD;
        bits = std::make_unique<std::atomic<uint64_t>[]>(words);
        for (size_t i = 0; i < words; ++i) {
            bits[i].store(0ULL, std::memory_order_relaxed);
        }
    }

    // Called under dirtyUnitTreeMutex. Atomically sets all bits in [idx, idx+num).
    void Set(size_t idx, size_t num)
    {
        for (size_t i = idx, end = idx + num; i < end; ++i) {
            bits[i >> LOG2_BITS_PER_WORD].fetch_or(1ULL << (i & BIT_INDEX_MASK), std::memory_order_relaxed);
        }
    }

    // Called under dirtyUnitTreeMutex. Atomically clears all bits in [idx, idx+num).
    void Clear(size_t idx, size_t num)
    {
        for (size_t i = idx, end = idx + num; i < end; ++i) {
            bits[i >> LOG2_BITS_PER_WORD].fetch_and(~(1ULL << (i & BIT_INDEX_MASK)), std::memory_order_relaxed);
        }
    }

    // Atomic lock-free read. Returns true if [idx, idx+num) is address-contiguous
    // with an existing dirty block (can be MergeInsert'd without creating an isolated node).
    bool CanMerge(size_t idx, size_t num) const
    {
        bool leftOk  = (idx > 0)               && IsSet(idx - 1);
        bool rightOk = (idx + num < totalUnits) && IsSet(idx + num);
        return leftOk || rightOk;
    }

private:
    static constexpr size_t BITS_PER_WORD = 64;          // number of bits in one uint64_t word
    static constexpr size_t LOG2_BITS_PER_WORD = 6;      // log2(BITS_PER_WORD), used for shift-based division
    static constexpr size_t BIT_INDEX_MASK = BITS_PER_WORD - 1; // mask for extracting bit offset within a word

    bool IsSet(size_t idx) const
    {
        uint64_t word = bits[idx >> LOG2_BITS_PER_WORD].load(std::memory_order_relaxed);
        return (word >> (idx & BIT_INDEX_MASK)) & 1ULL;
    }

    std::unique_ptr<std::atomic<uint64_t>[]> bits;
    size_t totalUnits = 0;
};

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
        dirtyBitmap.Init(regionCnt);
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
                bool dirtyOk = dirtyUnitTree.TakeUnitsLowAddr(num, idx);
                if (dirtyOk) {
                    DLOG(REGION, "c-tree %p alloc dirty units[%u+%u, %u) @[0x%zx, 0x%zx), %u dirty-units left",
                        &dirtyUnitTree, idx, num, idx + num, RegionInfo::GetUnitAddress(idx),
                        RegionInfo::GetUnitAddress(idx + num), dirtyUnitTree.GetTotalCount());

                    // Sync bitmap: clear taken units (under dirtyUnitTreeMutex).
                    dirtyBitmap.Clear(idx, num);
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
                bool releasedOk = releasedUnitTree.TakeUnitsLowAddr(num, idx);
                if (releasedOk) {
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
        // Sync bitmap: mark inserted units (under dirtyUnitTreeMutex).
        dirtyBitmap.Set(idx, num);
    }

    // Returns true if [idx, idx+num) is address-contiguous with an existing dirty block.
    // Lock-free best-effort check; safe to call without dirtyUnitTreeMutex.
    bool CanMergeWithDirty(UnitIndex idx, UnitCount num) const
    {
        return dirtyBitmap.CanMerge(idx, num);
    }

    void AddReleaseUnits(UnitIndex idx, UnitCount num)
    {
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        if (UNLIKELY(!releasedUnitTree.MergeInsert(idx, num, true))) {
            LOG(RTLOG_FATAL, "tid %d: failed to add release units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }

    UnitCount GetDirtyUnitCount() const
    {
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        return dirtyUnitTree.GetTotalCount();
    }

    UnitCount GetReleasedUnitCount() const
    {
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        return releasedUnitTree.GetTotalCount();
    }

    // Return the largest contiguous block in released/dirty tree (root node = max-heap top)
    UnitCount GetReleasedMaxBlock() const
    {
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        const auto* r = releasedUnitTree.RootNode();
        return r ? r->GetCount() : 0;
    }
    UnitCount GetDirtyMaxBlock() const
    {
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        const auto* r = dirtyUnitTree.RootNode();
        return r ? r->GetCount() : 0;
    }
    size_t GetReleasedNodeCount() const
    {
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        return releasedUnitTree.GetNodeCount();
    }
    size_t GetDirtyNodeCount() const
    {
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        return dirtyUnitTree.GetNodeCount();
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

    // physical pages of released units are probably released and they are prepared for allocation.
    mutable std::mutex releasedUnitTreeMutex;
    CartesianTree releasedUnitTree;

    // dirty units are neither cleared nor released, thus must be zeroed explicitly for allocation.
    mutable std::mutex dirtyUnitTreeMutex;
    CartesianTree dirtyUnitTree;

    // Bitmap mirroring dirtyUnitTree membership for O(1) merge-ability check.
    DirtyBitmap dirtyBitmap;
};
} // namespace MapleRuntime
#endif // MRT_FREE_REGION_MANAGER_H
