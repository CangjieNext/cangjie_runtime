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

#include "Heap/MAllocator/MSFreeManager.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Common/ScopedObjectAccess.h"
#include "Base/LogFile.h"
#include "Base/Panic.h"

namespace MapleRuntime {

class CentralRegionPool;

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
// FreeManager wraps the unit-based free-tree backend behind a region-aligned
// interface so MSGC callers still operate in MS_REGION_SIZE chunks.
//
class MSRegionManager {
    using UnitIndex = FreeManager::UnitIndex;
    using UnitCount = FreeManager::UnitCount;
    using RegionIndex = FreeManager::RegionIndex;
    using RegionCount = FreeManager::RegionCount;
    enum class AllocSource : uint8_t {
        RELEASED,
        BUMP,
    };

public:
    struct HeapRssBreakdown {
        size_t regionInfoCount = 0;
        size_t regionInfoBytes = 0;
        size_t regionBitmapBytes = 0;
        size_t releasedTreeMetadataBytes = 0;
        size_t releasedTreeNodeBytes = 0;
    };

    enum class HeuTriggerKind : uint8_t {
        SMALL_ALLOC,
        LARGE_ALLOC,
        BG_SWEEP_EXHAUSTED,
    };



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
    MSRegionInfo* TakeRegion(uint8_t cls, const CentralRegionPool& centralPool);

    // Take N units for large object (Tier 1, > threshold but <= 4MB).
    // unitCount = ceil(allocSize, UNIT_SIZE).
    // Returns pair: (MSRegionInfo* baseInfo, uintptr_t regionStart), or (nullptr, 0).
    MSRegionInfo* TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart);

    // Reclaim a small/medium Region (MS_REGION_SIZE).
    // In MSGC small-region steady-state this path is mostly bypassed by the
    // CentralRegionPool empty-cache. If it is used, the region goes directly
    // to releasedTree instead of a dirty cache.
    void ReclaimRegion(MSRegionInfo* region);

    // Reclaim a large Region spanning regionBytes.
    // regionStart is the actual start address of the allocation (from TakeRegionForLarge).
    void ReclaimRegionLarge(uintptr_t regionStart, size_t regionBytes);
    // Move a classless empty-cache region to releasedTree, releasing physical
    // pages first. Used by CentralRegionPool's background scavenger.
    void ReclaimEmptyRegionToReleased(MSRegionInfo* region);

    // Compatibility hook for allocator reclaim. MSGC small-region reclaim is
    // handled by CentralRegionPool empty-cache scavenging, so this is a no-op.
    size_t ReleaseGarbageRegions(size_t targetCachedSize);

    // Try to grow the committed heap by one segment.
    // Called when inactiveZone reaches committedEnd_.
    // Returns true if growth succeeded, false if already at max.
    bool GrowOneSegment();

    // Shrink the committed heap by reclaiming trailing released units
    // contiguous with inactiveZone_, then reducing committedEnd_ by whole segments.
    // Called after scavenging empty-cache / releasedTree in the gc-helper thread.
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
    uintptr_t GetTailAllocCursor() const { return tailAllocCursor_.load(std::memory_order_relaxed); }
    size_t GetMaxRegionCount() const { return committedRegionCount_; }
    size_t GetMaxVirtualRegionCount() const { return maxVirtualRegionCount_; }
    size_t GetCurrentCapacityBytes() const
    {
        if (tailDirectReleaseValidation_) {
            uintptr_t tailCursor = tailAllocCursor_.load(std::memory_order_relaxed);
            return (committedEnd_ > tailCursor) ? (committedEnd_ - tailCursor) : 0;
        }
        uintptr_t inactive = inactiveZone_.load(std::memory_order_relaxed);
        return (inactive > heapStart_) ? (inactive - heapStart_) : 0;
    }
    size_t GetVirginRegionCount() const
    {
        if (tailDirectReleaseValidation_) {
            uintptr_t tailCursor = tailAllocCursor_.load(std::memory_order_relaxed);
            return (tailCursor > heapStart_) ? ((tailCursor - heapStart_) / MS_REGION_SIZE) : 0;
        }
        uintptr_t inactive = inactiveZone_.load(std::memory_order_relaxed);
        return (committedEnd_ > inactive) ? ((committedEnd_ - inactive) / MS_REGION_SIZE) : 0;
    }
    bool UseTailDirectReleaseValidation() const { return tailDirectReleaseValidation_; }

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

    // Free unit count across releasedTree + unallocated zone (within committed region).
    uint32_t GetFreeUnitCount() const
    {
        if (tailDirectReleaseValidation_) {
            uintptr_t tailCursor = tailAllocCursor_.load(std::memory_order_relaxed);
            return (tailCursor > heapStart_) ? static_cast<uint32_t>((tailCursor - heapStart_) / unitSize_) : 0;
        }
        uint32_t releasedUnits = releasedFreeManager_.GetReleasedUnitCount();
        uintptr_t committed = committedEnd_;
        uintptr_t inactive = inactiveZone_.load(std::memory_order_relaxed);
        uint32_t inactiveUnits = (committed > inactive)
            ? static_cast<uint32_t>((committed - inactive) / unitSize_)
            : 0;
        return releasedUnits + inactiveUnits;
    }

    size_t GetReleasedTreeBytes() const
    {
        return releasedFreeManager_.GetReleasedBytes();
    }

    HeapRssBreakdown GetHeapRssBreakdown() const;
    void DumpReleasedTreeSpans();

    uint32_t GetReleasedTreeNodeCount() const { return releasedFreeManager_.GetNodeCount(); }
    uint32_t GetReleasedTreeMaxBlock() const { return releasedFreeManager_.GetMaxBlockRegions(); }
    size_t GetDirtyTreeBytes() const { return 0; } // stub: no dirty tree in current design
    uint64_t GetHeapGrowCount() const { return heapGrowCount_; }

    void DumpAndResetAllocClearStats();

    // Unified layer-1 HEU trigger entry.
    // - SMALL_ALLOC: called after taking a fresh small-object region.
    // - LARGE_ALLOC: called after taking a large-object span.
    // - BG_SWEEP_EXHAUSTED: called when layer-2 has drained unswept backlog.
    void MaybeNotifyMark(HeuTriggerKind triggerKind, const CentralRegionPool* centralPool = nullptr,
                           uint8_t cls = 0, bool bumpSource = true);

    // Layer-2 trigger: notify gc-helper to sweep if the given size class
    // has low ready supply but unswept backlog.  When called without
    // centralPool (e.g. from GC_CONCURRENT_SWEEP) it is an unconditional kick.
    void MaybeNotifySweep(const CentralRegionPool* centralPool = nullptr, uint8_t cls = 0);

    // Layer-3 trigger: notify gc-helper to reclaim empty regions back to OS.
    void MaybeNotifyReclaim();

private:
    // Take contiguous regions from releasedTree or inactiveZone bump.
    // FreeManager keeps released space region-aligned while allowing the backend
    // implementation underneath to remain unit-based and replaceable.
    bool TakeRegions(RegionCount num, RegionIndex& idx, AllocSource* source = nullptr);
    bool TakeTailRegionsForValidation(RegionCount num, RegionIndex& idx, AllocSource* source = nullptr);

    // Reclaim regions to released tree (release physical pages first).
    void ReclaimRegionsReleased(RegionIndex idx, RegionCount num);
    void ReclaimRegionsDirectForValidation(RegionIndex idx, RegionCount num);

    // Release physical pages at given unit range (platform-specific).
    void ReleaseUnits(UnitIndex idx, UnitCount num) const;

    // Clear units (zero-fill) at given index/count.
    void ClearUnits(UnitIndex idx, UnitCount num) const;

    // --- Region ↔ Unit / Address conversion helpers ---
    uintptr_t RegionIdxToAddr(RegionIndex idx) const
    {
        return heapStart_ + static_cast<uintptr_t>(idx) * MS_REGION_SIZE;
    }

    RegionIndex AddrToRegionIdx(uintptr_t addr) const
    {
        return static_cast<RegionIndex>((addr - heapStart_) / MS_REGION_SIZE);
    }

    UnitIndex RegionToUnitIdx(RegionIndex regIdx) const
    {
        return releasedFreeManager_.RegionToUnitIdx(regIdx);
    }

    UnitCount RegionCountToUnitCount(RegionCount regCount) const
    {
        return releasedFreeManager_.RegionCountToUnitCount(regCount);
    }

    RegionIndex UnitToRegionIdx(UnitIndex unitIdx) const
    {
        return releasedFreeManager_.UnitToRegionIdx(unitIdx);
    }

    RegionCount UnitCountToRegionCount(UnitCount unitCount) const
    {
        return releasedFreeManager_.UnitCountToRegionCount(unitCount);
    }

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
    std::atomic<uintptr_t> tailAllocCursor_{0};
    bool tailDirectReleaseValidation_ = false;

    // Mutex for heap growth (serializes GrowOneSegment calls).
    std::mutex growMutex_;

    // Region-aligned adapter over a replaceable free-space backend.
    FreeManager releasedFreeManager_;

    // O6 observability: allocation-source hit counts and ClearUnits costs.
    std::atomic<uint64_t> allocFromReleasedCount_{0};
    std::atomic<uint64_t> allocFromBumpCount_{0};

    std::atomic<uint64_t> clearUnitsCallCount_{0};
    std::atomic<uint64_t> clearUnitsBytes_{0};
    std::atomic<uint64_t> clearUnitsTimeNs_{0};
    std::atomic<uint64_t> statsWindowStartNs_{0};
    std::atomic<uint64_t> lastHeuRequestNs_{0};

    uint64_t heapGrowCount_ = 0;  // Number of GrowOneSegment() calls
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_REGION_MANAGER_H
