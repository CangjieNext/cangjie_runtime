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
// Only the currently committed heap range is considered usable. When the
// inactive-zone bump allocator reaches the committed end, FreeManager drives
// heap growth and asks MSRegionManager to initialize the corresponding
// MSRegionInfo entries for the newly committed range.
//
// MSRegionInfo[] is placed at the mmap front (forward array, not mirror/inverted).
// Each MSRegionInfo is 64 bytes, exactly one cache line.
// MetadataSize = ceil(regionCount * 64, PAGE_SIZE).
//
// CartesianTree operates in REGION indexes (each Index = one MS_REGION_SIZE region).
// FreeManager owns released-span page return; MSRegionManager keeps only ClearUnits locally.
//
class MSRegionManager : private FreeManager::HeapRangeHandler {
    using UnitIndex = FreeManager::UnitIndex;
    using UnitCount = FreeManager::UnitCount;
    using RegionIndex = FreeManager::RegionIndex;
    using RegionCount = FreeManager::RegionCount;
    enum class AllocSource : uint8_t {
        RELEASED,
        BUMP,
    };

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
    MSRegionInfo* TakeRegion(uint8_t cls, const CentralRegionPool& centralPool);

    // Take N units for large object (Tier 1, > threshold but <= 4MB).
    // unitCount = ceil(allocSize, UNIT_SIZE).
    // Returns pair: (MSRegionInfo* baseInfo, uintptr_t regionStart), or (nullptr, 0).
    MSRegionInfo* TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart,
                                     const CentralRegionPool* centralPool = nullptr);

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
    // The heap-top state is managed by FreeManager; this method delegates to it.
    // Returns true if growth succeeded, false if already at max.
    bool GrowOneSegment();

    // Shrink the committed heap after scavenging empty-cache / releasedTree.
    // The actual trailing-tail reclaim and committed-end update are managed by FreeManager.
    void ShrinkHeap();

    // O(1) address → MSRegionInfo lookup (region index = (addr - heapStart) / MS_REGION_SIZE).
    MSRegionInfo* GetRegionInfoAt(uintptr_t addr) const
    {
        size_t regionIdx = (addr - heapStart_) / MS_REGION_SIZE;
        return &array_[regionIdx];
    }

    uintptr_t GetHeapStart() const { return heapStart_; }
    uintptr_t GetHeapEnd() const { return freeManager_.GetCommittedEnd(); }
    uintptr_t GetMaxHeapEnd() const { return freeManager_.GetMaxHeapEnd(); }
    size_t GetCurrentCapacityBytes() const { return freeManager_.GetCurrentCapacityBytes(); }
    uintptr_t GetInactiveZone() const { return freeManager_.GetInactiveZone(); }
    size_t GetMaxRegionCount() const { return freeManager_.GetCommittedRegionCount(); }
    size_t GetMaxVirtualRegionCount() const { return freeManager_.GetMaxVirtualRegionCount(); }

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
    size_t GetMaxUnitCount() const { return freeManager_.GetCommittedUnitCount(); }

    // Total unit count for the full virtual reservation.
    size_t GetMaxVirtualUnitCount() const { return freeManager_.GetMaxVirtualUnitCount(); }

    // Free unit count across releasedTree + unallocated zone (within committed region).
    uint32_t GetFreeUnitCount() const
    {
        return static_cast<uint32_t>(freeManager_.GetFreeUnitCount());
    }

    size_t GetReleasedTreeBytes() const
    {
        return freeManager_.GetReleasedBytes();
    }

    uint32_t GetReleasedTreeNodeCount() const { return freeManager_.GetNodeCount(); }
    uint32_t GetReleasedTreeMaxBlock() const { return freeManager_.GetMaxBlockRegions(); }
    size_t GetDirtyTreeBytes() const { return 0; } // stub: no dirty tree in current design
    uint64_t GetHeapGrowCount() const { return freeManager_.GetHeapGrowCount(); }

    void DumpAndResetAllocClearStats();

    // Unified layer-1 HEU trigger entry. MSGC now only keeps the TAKE_REGIONS path.
    void MaybeNotifyMark();

    // Layer-2 trigger: notify gc-helper to sweep only when the target size
    // class has both low ready supply and unswept backlog.
    void MaybeNotifySweep(const CentralRegionPool* centralPool, uint8_t cls);

    // Layer-3 trigger: notify gc-helper to reclaim empty regions back to OS.
    void MaybeNotifyReclaim();

    // Inline reclaim: when TakeRegion expands the heap via BUMP, synchronously
    // release one empty-cache region to keep committed bytes stable.
    void InlineReclaimForBalance(const CentralRegionPool& pool);

private:
    void InitializeFreshRegions(RegionIndex startIdx, RegionCount count) override;
    void OnCommittedHeapExtended(uintptr_t newCommittedEnd) override;

    // Take contiguous regions from releasedTree or inactiveZone bump.
    // High-level orchestration stays in MSRegionManager; FreeManager only exposes
    // the primitive heap-state operations used by this path.
    bool TakeRegions(RegionCount num, RegionIndex& idx, AllocSource* source = nullptr);

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
        return static_cast<UnitIndex>(static_cast<size_t>(regIdx) * unitsPerMSRegion_);
    }

    UnitCount RegionCountToUnitCount(RegionCount regCount) const
    {
        return static_cast<UnitCount>(static_cast<size_t>(regCount) * unitsPerMSRegion_);
    }

    MSRegionInfo* array_ = nullptr;         // MSRegionInfo array at mmap front
    uintptr_t heapStart_ = 0;              // First heap region address (immutable)
    size_t unitSize_ = 0;                  // = UNIT_SIZE (system page size)
    size_t unitsPerMSRegion_ = 0;          // = MS_REGION_SIZE / UNIT_SIZE

    // Unified released-span entry for MSGC heap management.
    FreeManager freeManager_;

    // O6 observability: allocation-source hit counts and ClearUnits costs.
    std::atomic<uint64_t> allocFromReleasedCount_{0};
    std::atomic<uint64_t> allocFromBumpCount_{0};

    std::atomic<uint64_t> clearUnitsCallCount_{0};
    std::atomic<uint64_t> clearUnitsBytes_{0};
    std::atomic<uint64_t> clearUnitsTimeNs_{0};
    std::atomic<uint64_t> statsWindowStartNs_{0};
    std::atomic<uint64_t> lastHeuRequestNs_{0};
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_REGION_MANAGER_H
