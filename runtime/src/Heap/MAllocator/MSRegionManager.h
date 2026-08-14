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
#include "Heap/MAllocator/MSShardRouting.h"
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
// FreeManager owns Region-index allocation metadata and released-span page
// return; MSRegionManager keeps only MSRegionInfo initialization/reset local.
//
class MSRegionManager : private FreeManager::HeapRangeHandler {
    using UnitIndex = FreeManager::UnitIndex;
    using UnitCount = FreeManager::UnitCount;
    using RegionIndex = FreeManager::RegionIndex;
    using RegionCount = FreeManager::RegionCount;
public:
    MSRegionManager() = default;
    ~MSRegionManager();

    // Checked form used by runtime initialization. Validates heap alignment,
    // RegionIndex capacity, metadata multiplication/alignment and final sum.
    static bool TryComputeVirtualReserveLayout(size_t heapSize,
                                               size_t& heapAligned,
                                               size_t& metadataSize,
                                               size_t& totalSize);

    // Initialize MSRegionManager with virtual reservation.
    // mmapBase is the start of the whole mmap area (size = totalSize returned
    // by TryComputeVirtualReserveLayout).
    // maxVirtualHeapSize is the total virtual heap capacity.
    void Initialize(uintptr_t mmapBase, size_t maxVirtualHeapSize);

    // Take one MS Region for mutator fresh allocation.
    MSRegionInfo* TakeRegion(uint16_t homeShardId = kMSInvalidShardId);

    // Take N units for a large object. Every allocation above the small-object
    // region threshold uses this contiguous-Region path.
    // unitCount = ceil(allocSize, UNIT_SIZE).
    // Returns pair: (MSRegionInfo* baseInfo, uintptr_t regionStart), or (nullptr, 0).
    MSRegionInfo* TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart,
                                     uint16_t homeShardId = kMSInvalidShardId);

    // Reclaim a large Region spanning regionBytes.
    // regionStart is the actual start address of the allocation (from TakeRegionForLarge).
    void ReclaimRegionLarge(uintptr_t regionStart, size_t regionBytes);
    void ReclaimReleasedRegionsToHotFreeBatch(const std::vector<FreeManager::RegionSpan>& spans);

    // Try to grow the committed heap by one segment.
    // The heap-top state is managed by FreeManager; this method delegates to it.
    // Returns true if growth succeeded, false if already at max.
    bool GrowOneSegment();
    // Scavenge up to maxBytes of hot-free regions (pages returned to OS).
    // Used by BackgroundScavengeLoop for per-quantum budget control.
    size_t ScavengeFreeRegionsUpTo(size_t maxBytes);
    void AdvanceScavengeEpoch(uint32_t epoch) { freeManager_.AdvanceScavengeEpoch(epoch); }
    size_t GetSegmentSizeBytes() const { return freeManager_.GetSegmentSizeBytes(); }

    // Shrink the committed heap after background reclaim/scavenge has exposed trailing hot-free space.
    // The actual trailing-tail reclaim and committed-end update are managed by FreeManager.
    void ShrinkHeap();
    bool HasPendingShrinkWork(const CentralRegionPool* centralPool) const;
    size_t ShrinkOneIdleSegment(CentralRegionPool* centralPool);

    uintptr_t GetHeapStart() const { return heapStart_; }
    uintptr_t GetHeapEnd() const { return freeManager_.GetCommittedEnd(); }
    uintptr_t GetMaxHeapEnd() const { return freeManager_.GetMaxHeapEnd(); }
    size_t GetCurrentCapacityBytes() const { return freeManager_.GetCurrentCapacityBytes(); }
    uintptr_t GetInactiveZone() const { return freeManager_.GetInactiveZone(); }
    size_t GetMaxRegionCount() const { return freeManager_.GetCommittedRegionCount(); }
    size_t GetMaxVirtualRegionCount() const { return freeManager_.GetMaxVirtualRegionCount(); }

    // Get the base pointer to the MSRegionInfo array (at mmap front).
    MSRegionInfo* GetArrayBase() const { return array_; }

    // O(1) compute unit address from unit index.
    uintptr_t GetUnitAddress(UnitIndex idx) const
    {
        return heapStart_ + static_cast<uintptr_t>(idx) * unitSize_;
    }

    uint32_t GetShardCount() const { return freeManager_.GetShardCount(); }

    // Free unit count currently tracked by FreeManager inside the committed heap.
    uint32_t GetFreeUnitCount() const
    {
        return static_cast<uint32_t>(freeManager_.GetFreeUnitCount());
    }
    // Unified layer-1 HEU trigger entry. MSGC now only keeps the TAKE_REGIONS path.
    void MaybeNotifyMark();

    // Layer-4 trigger: notify gc-helper to scavenge heap when retained exceeds
    // threshold and enough releasable supply is available.
    void MaybeNotifyScavenge();

    // Return bytes in hot-free regions (free but pages not yet returned to OS).
    size_t GetHotFreeBytes() const;
    size_t GetHotFreeBytesReference() const;
    uint16_t ResolveHomeShardId(uint16_t shardHint = kMSInvalidShardId) const;

private:
    void InitializeFreshRegions(RegionIndex startIdx, RegionCount count) override;
    void OnCommittedHeapExtended(uintptr_t newCommittedEnd) override;

    // Take contiguous regions from FreeManager, growing the committed heap on demand.
    // High-level orchestration stays in MSRegionManager; FreeManager only exposes
    // the primitive heap-state operations used by this path.
    bool TakeRegions(RegionCount num, uint16_t homeShardId, RegionIndex& idx);

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

    std::atomic<bool> scavengePressureProbeInProgress_ {false};
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_REGION_MANAGER_H
