// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_REGION_MANAGER_H
#define MRT_MS_REGION_MANAGER_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <mutex>
#include <cstddef>
#include <cstdint>

#include "CartesianTree.h"
#include "MSRegionInfo.h"
#include "Common/ScopedObjectAccess.h"
#include "Base/LogFile.h"
#include "Base/Panic.h"

namespace MapleRuntime {

// MSRegionManager: independent region manager for Mark-Sweep GC mode.
//
// Memory layout (Phase 2):
//   [MSRegionInfo[] array | HeapRegion space]
//   ^mmap base            ^heapStart        ^heapEnd
//
// MSRegionInfo[] is placed at the mmap front (forward array, not mirror/inverted).
// Each MSRegionInfo is 64 bytes, exactly one cache line.
// MetadataSize = ceil(regionCount * 64, PAGE_SIZE).
//
// CartesianTree operates in UNIT indexes (UNIT_SIZE = system page size).
// Each MS Region = MS_REGION_SIZE / UNIT_SIZE units (e.g., 2 on macOS, 8 on Linux-4KB).
//
class MSRegionManager {
    using UnitIndex = CartesianTree::Index;
    using UnitCount = CartesianTree::Count;

public:
    MSRegionManager() = default;
    ~MSRegionManager();

    // Calculate total mmap size needed: metadata (MSRegionInfo[]) + heap.
    static size_t GetHeapMemorySize(size_t heapSize);

    // Calculate metadata size for MSRegionInfo array.
    static size_t GetMetadataSize(size_t heapSize);

    // Calculate the number of MS regions that fit in given heap size.
    static size_t GetRegionCount(size_t heapSize);

    // Initialize MSRegionManager from mmap base.
    // mmapBase is the start of the whole mmap area.
    // heapSize is the usable heap size (before rounding).
    void Initialize(size_t heapSize, uintptr_t mmapBase);

    // Take one MS Region (MS_REGION_SIZE bytes).
    // Returns the MSRegionInfo* for the new region, or nullptr if OOM.
    MSRegionInfo* TakeRegion();

    // Take N units for large object (Tier 1, > threshold but <= 4MB).
    // unitCount = ceil(allocSize, UNIT_SIZE).
    // Returns pair: (MSRegionInfo* baseInfo, uintptr_t regionStart), or (nullptr, 0).
    MSRegionInfo* TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart);

    // Reclaim a small/medium Region (MS_REGION_SIZE).
    // Adds units back to dirty tree.
    void ReclaimRegion(MSRegionInfo* region);

    // Reclaim a large Region spanning regionBytes.
    // regionStart is the actual start address of the allocation (from TakeRegionForLarge).
    void ReclaimRegionLarge(uintptr_t regionStart, size_t regionBytes);

    // O(1) address → MSRegionInfo lookup (region index = (addr - heapStart) / MS_REGION_SIZE).
    MSRegionInfo* GetRegionInfoAt(uintptr_t addr) const
    {
        size_t regionIdx = (addr - heapStart_) / MS_REGION_SIZE;
        return &array_[regionIdx];
    }

    uintptr_t GetHeapStart() const { return heapStart_; }
    uintptr_t GetHeapEnd() const { return heapEnd_; }
    uintptr_t GetInactiveZone() const { return inactiveZone_.load(std::memory_order_relaxed); }
    size_t GetMaxRegionCount() const { return maxRegionCount_; }

    // Get the base pointer to the MSRegionInfo array (at mmap front).
    MSRegionInfo* GetArrayBase() const { return array_; }

    // Units per MS Region (= MS_REGION_SIZE / UNIT_SIZE).
    size_t GetUnitsPerMSRegion() const { return unitsPerMSRegion_; }

    // Large object threshold (= MS_REGION_SIZE for MS mode).
    size_t GetLargeObjectThreshold() const { return MS_REGION_SIZE; }

    // O(1) compute unit address from unit index.
    uintptr_t GetUnitAddress(UnitIndex idx) const
    {
        return heapStart_ + static_cast<uintptr_t>(idx) * unitSize_;
    }

    // Total unit count for the heap.
    size_t GetMaxUnitCount() const { return maxUnitCount_; }

    // Free unit count across both trees + unallocated zone.
    uint32_t GetFreeUnitCount() const
    {
        uint32_t dirty = dirtyTree_.GetTotalCount();
        uint32_t released = releasedTree_.GetTotalCount();
        uint32_t inactive = (heapEnd_ > inactiveZone_.load(std::memory_order_relaxed))
            ? static_cast<uint32_t>((heapEnd_ - inactiveZone_.load(std::memory_order_relaxed)) / unitSize_)
            : 0;
        return dirty + released + inactive;
    }

private:
    // Try to take units from dirty tree, then released tree, then extend inactiveZone.
    // Returns unit index via 'idx'. Returns true on success.
    bool TakeUnitsInternal(UnitCount num, UnitIndex& idx);

    // Take units aligned to 'alignment' from dirty/released trees with
    // over-allocation + split.  Falls back to inactiveZone which is naturally aligned.
    bool TakeUnitsAligned(UnitCount num, UnitCount alignment, UnitIndex& idx);

    // Reclaim units back to dirty tree.
    void ReclaimUnitsInternal(UnitIndex idx, UnitCount num);

    // Clear units (zero-fill) at given index/count.
    void ClearUnits(UnitIndex idx, UnitCount num) const;

    MSRegionInfo* array_ = nullptr;         // MSRegionInfo array at mmap front
    uintptr_t heapStart_ = 0;              // First heap region address
    uintptr_t heapEnd_ = 0;                // Past-the-end heap address
    size_t maxRegionCount_ = 0;            // Number of MSRegionInfo entries
    size_t maxUnitCount_ = 0;              // Total unit count for the heap
    size_t unitSize_ = 0;                  // = UNIT_SIZE (system page size)
    size_t unitsPerMSRegion_ = 0;          // = MS_REGION_SIZE / UNIT_SIZE

    // Heap watermark: everything below inactiveZone_ has been allocated at least once.
    std::atomic<uintptr_t> inactiveZone_{0};

    // Dual CartesianTree for free unit management (same design as FreeRegionManager).
    // dirty: units that have been used and reclaimed (contain stale data).
    // released: units whose physical pages have been released (madvise DONTNEED).
    std::mutex dirtyTreeMutex_;
    CartesianTree dirtyTree_;

    std::mutex releasedTreeMutex_;
    CartesianTree releasedTree_;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_REGION_MANAGER_H
