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
// Memory layout (virtual reserve + segmented commit):
//   [MSRegionInfo[maxVirtual] | HeapRegion space (maxVirtual)]
//   ^mmap base                ^heapStart   ^committedEnd  ^maxHeapEnd
//
// At initialization a large virtual address space is reserved (e.g. 4GB) via
// mmap PROT_READ|PROT_WRITE (physical pages are lazily committed by the OS).
// Only `committedEnd_` bytes of heap are considered usable.  When the
// inactiveZone bump allocator reaches committedEnd_, GrowOneSegment()
// extends it by segmentSize_ bytes and initializes the corresponding
// MSRegionInfo entries.
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

    // Calculate total mmap size needed for the maximum virtual reservation.
    // This is the size passed to MemMap::MapMemory as reqSize.
    static size_t GetVirtualReserveSize(size_t maxVirtualHeapSize);

    // Calculate total mmap size needed: metadata (MSRegionInfo[]) + heap.
    // (Legacy helper — used for the initial committed segment sizing.)
    static size_t GetHeapMemorySize(size_t heapSize);

    // Calculate metadata size for MSRegionInfo array.
    static size_t GetMetadataSize(size_t heapSize);

    // Calculate the number of MS regions that fit in given heap size.
    static size_t GetRegionCount(size_t heapSize);

    // Compute the maximum virtual heap size given the user-requested heapSize.
    // Capped at 4GB.
    static size_t ComputeMaxVirtualHeapSize(size_t heapSize);

    // Initialize MSRegionManager with virtual reservation.
    // mmapBase is the start of the whole mmap area (size = GetVirtualReserveSize).
    // heapSize is the initial usable heap size (first committed segment).
    // maxVirtualHeapSize is the total virtual heap capacity.
    void Initialize(size_t heapSize, uintptr_t mmapBase, size_t maxVirtualHeapSize);

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

    // Batch migrate dirty units to released tree, releasing physical pages.
    // Keeps up to targetCachedSize bytes in dirtyTree as cache.
    // Returns total bytes released.
    size_t ReleaseGarbageRegions(size_t targetCachedSize);

    // Try to grow the committed heap by one segment.
    // Called when inactiveZone reaches committedEnd_.
    // Returns true if growth succeeded, false if already at max.
    bool GrowOneSegment();

    // Shrink the committed heap by reclaiming trailing released units
    // contiguous with inactiveZone_, then reducing committedEnd_ by whole segments.
    // Called after ReleaseGarbageRegions in the gc-helper thread.
    void ShrinkHeap();

    // O(1) address → MSRegionInfo lookup (region index = (addr - heapStart) / MS_REGION_SIZE).
    MSRegionInfo* GetRegionInfoAt(uintptr_t addr) const
    {
        size_t regionIdx = (addr - heapStart_) / MS_REGION_SIZE;
        return &array_[regionIdx];
    }

    uintptr_t GetHeapStart() const { return heapStart_; }
    uintptr_t GetHeapEnd() const { return committedEnd_; }
    uintptr_t GetMaxHeapEnd() const { return maxHeapEnd_; }
    uintptr_t GetInactiveZone() const { return inactiveZone_.load(std::memory_order_relaxed); }
    size_t GetMaxRegionCount() const { return committedRegionCount_; }
    size_t GetMaxVirtualRegionCount() const { return maxVirtualRegionCount_; }

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

    // Total unit count for the committed heap.
    size_t GetMaxUnitCount() const { return committedUnitCount_; }

    // Total unit count for the full virtual reservation.
    size_t GetMaxVirtualUnitCount() const { return maxVirtualUnitCount_; }

    // Free unit count across both trees + unallocated zone (within committed region).
    uint32_t GetFreeUnitCount() const
    {
        uint32_t dirty = dirtyTree_.GetTotalCount();
        uint32_t released = releasedTree_.GetTotalCount();
        uintptr_t committed = committedEnd_;
        uintptr_t inactive = inactiveZone_.load(std::memory_order_relaxed);
        uint32_t inactiveUnits = (committed > inactive)
            ? static_cast<uint32_t>((committed - inactive) / unitSize_)
            : 0;
        return dirty + released + inactiveUnits;
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

    // Reclaim units to released tree (release physical pages first).
    void ReclaimUnitsReleased(UnitIndex idx, UnitCount num);

    // Release physical pages at given unit range (platform-specific).
    void ReleaseUnits(UnitIndex idx, UnitCount num) const;

    // Clear units (zero-fill) at given index/count.
    void ClearUnits(UnitIndex idx, UnitCount num) const;

    MSRegionInfo* array_ = nullptr;         // MSRegionInfo array at mmap front
    uintptr_t heapStart_ = 0;              // First heap region address (immutable)
    uintptr_t maxHeapEnd_ = 0;             // Virtual reservation upper bound (immutable)
    uintptr_t committedEnd_ = 0;           // Current committed heap upper bound (grows)
    size_t maxVirtualRegionCount_ = 0;     // Total MSRegionInfo entries (for max virtual)
    size_t maxVirtualUnitCount_ = 0;       // Total unit count for max virtual heap
    size_t committedRegionCount_ = 0;      // MSRegionInfo entries for committed heap
    size_t committedUnitCount_ = 0;        // Unit count for committed heap
    size_t segmentSize_ = 0;              // Growth segment size in bytes
    size_t unitSize_ = 0;                  // = UNIT_SIZE (system page size)
    size_t unitsPerMSRegion_ = 0;          // = MS_REGION_SIZE / UNIT_SIZE

    // Heap watermark: everything below inactiveZone_ has been allocated at least once.
    std::atomic<uintptr_t> inactiveZone_{0};

    // Mutex for heap growth (serializes GrowOneSegment calls).
    std::mutex growMutex_;

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
