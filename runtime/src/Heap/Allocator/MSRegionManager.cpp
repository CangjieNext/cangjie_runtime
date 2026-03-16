// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MSRegionManager.h"

#include <cstring>
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "AllocUtil.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"
#include "Collector/Collector.h"

namespace MapleRuntime {

// External: system page size (defined in RegionManager.cpp).
extern const size_t MRT_PAGE_SIZE;

MSRegionManager::~MSRegionManager()
{
    dirtyTree_.Fini();
    releasedTree_.Fini();
    // array_ points into the mmap region — no free needed.
    array_ = nullptr;
}

// ============================================================================
// Static helpers
// ============================================================================

size_t MSRegionManager::GetRegionCount(size_t heapSize)
{
    heapSize = RoundUp<size_t>(heapSize, MS_REGION_SIZE);
    return heapSize / MS_REGION_SIZE;
}

size_t MSRegionManager::GetMetadataSize(size_t heapSize)
{
    size_t regionCount = GetRegionCount(heapSize);
    size_t rawSize = regionCount * sizeof(MSRegionInfo);
    return RoundUp<size_t>(rawSize, MRT_PAGE_SIZE);
}

size_t MSRegionManager::GetHeapMemorySize(size_t heapSize)
{
    size_t metadataSize = GetMetadataSize(heapSize);
    size_t heapAligned = RoundUp<size_t>(heapSize, MS_REGION_SIZE);
    return metadataSize + heapAligned;
}

// ============================================================================
// Initialize
// ============================================================================

void MSRegionManager::Initialize(size_t heapSize, uintptr_t mmapBase)
{
    unitSize_ = MRT_PAGE_SIZE;
    unitsPerMSRegion_ = MS_REGION_SIZE / unitSize_;

    size_t metadataSize = GetMetadataSize(heapSize);
    maxRegionCount_ = GetRegionCount(heapSize);
    size_t heapAligned = RoundUp<size_t>(heapSize, MS_REGION_SIZE);
    maxUnitCount_ = heapAligned / unitSize_;

    // Memory layout: [MSRegionInfo[] | Heap Regions]
    array_ = reinterpret_cast<MSRegionInfo*>(mmapBase);
    heapStart_ = mmapBase + metadataSize;
    heapEnd_ = heapStart_ + heapAligned;
    inactiveZone_.store(heapStart_, std::memory_order_relaxed);

    // Zero-initialize all MSRegionInfo entries
    memset(array_, 0, maxRegionCount_ * sizeof(MSRegionInfo));
    for (size_t i = 0; i < maxRegionCount_; i++) {
        array_[i].regionType = MSRegionType::MS_REGION_FREE;
        array_[i].nextRegionIdx = MSRegionInfo::INVALID_IDX;
        array_[i].prevRegionIdx = MSRegionInfo::INVALID_IDX;
    }

    // Initialize CartesianTrees with max unit count.
    dirtyTree_.Init(maxUnitCount_);
    releasedTree_.Init(maxUnitCount_);

    LOG(RTLOG_INFO, "[MS-GC] MSRegionManager initialized: mmapBase=0x%zx, metadataSize=%zuKB, "
        "heapStart=0x%zx, heapEnd=0x%zx, maxRegions=%zu, maxUnits=%zu, unitsPerMSRegion=%zu",
        mmapBase, metadataSize / 1024,
        heapStart_, heapEnd_, maxRegionCount_, maxUnitCount_, unitsPerMSRegion_);
}

// ============================================================================
// TakeUnitsInternal — 3-level allocation: dirtyTree → releasedTree → inactiveZone
// ============================================================================

bool MSRegionManager::TakeUnitsInternal(UnitCount num, UnitIndex& idx)
{
    bool tryDirtyTree = true;
    bool tryReleasedTree = true;

    while (tryDirtyTree || tryReleasedTree) {
        // 1. Try dirty tree first (data in memory, fastest).
        //    refreshRegionInfo=false: CopyGC's UnitInfo mirror is not used in MS
        //    mode; calling RefreshFreeRegionInfo would write into MSRegionInfo array.
        if (tryDirtyTree && dirtyTreeMutex_.try_lock()) {
            if (dirtyTree_.TakeUnitsLowestAddr(num, idx, false)) {
                dirtyTreeMutex_.unlock();
                ClearUnits(idx, num);
                return true;
            }
            tryDirtyTree = false;
            dirtyTreeMutex_.unlock();
        }

        // 2. Try released tree (pages may be zeroed by OS).
        if (tryReleasedTree && releasedTreeMutex_.try_lock()) {
            if (releasedTree_.TakeUnitsLowestAddr(num, idx, false)) {
                releasedTreeMutex_.unlock();
                return true;
            }
            tryReleasedTree = false;
            releasedTreeMutex_.unlock();
        }

        ScopedEnterSaferegion enterSaferegion(true);
    }

    // 3. Extend inactive zone (bump allocator for virgin heap space).
    size_t byteCount = static_cast<size_t>(num) * unitSize_;
    uintptr_t addr = inactiveZone_.fetch_add(byteCount, std::memory_order_relaxed);
    if (addr + byteCount <= heapEnd_) {
        idx = static_cast<UnitIndex>((addr - heapStart_) / unitSize_);
        return true;
    }
    // Rollback
    inactiveZone_.fetch_sub(byteCount, std::memory_order_relaxed);
    return false;
}

// ============================================================================
// TakeUnitsAligned — allocate units with alignment guarantee.
// Over-allocates from dirtyTree/releasedTree and returns unneeded fragments.
// Falls back to inactiveZone which is naturally aligned at initialization.
// ============================================================================

bool MSRegionManager::TakeUnitsAligned(UnitCount num, UnitCount alignment, UnitIndex& idx)
{
    // Fast path: try inactiveZone first (bump allocator, naturally aligned
    // because heapStart_ is page-aligned and only aligned allocs use this path).
    {
        size_t byteCount = static_cast<size_t>(num) * unitSize_;
        // Align the bump pointer to alignment * unitSize_ boundary
        uintptr_t alignBytes = static_cast<uintptr_t>(alignment) * unitSize_;
        uintptr_t oldAddr = inactiveZone_.load(std::memory_order_relaxed);
        uintptr_t alignedAddr;
        do {
            alignedAddr = ((oldAddr + alignBytes - 1) / alignBytes) * alignBytes;
            if (alignedAddr + byteCount > heapEnd_) {
                break;  // Not enough space, fall through to tree path
            }
        } while (!inactiveZone_.compare_exchange_weak(oldAddr, alignedAddr + byteCount,
                                                       std::memory_order_relaxed));
        if (alignedAddr + byteCount <= heapEnd_) {
            idx = static_cast<UnitIndex>((alignedAddr - heapStart_) / unitSize_);
            return true;
        }
    }

    // Slow path: allocate from dirty/released trees with over-allocation.
    UnitCount totalNeeded = num + alignment - 1;
    UnitIndex rawIdx = 0;

    bool tryDirtyTree = true;
    bool tryReleasedTree = true;
    bool needClear = false;

    while (tryDirtyTree || tryReleasedTree) {
        // refreshRegionInfo=false: see TakeUnitsInternal comment.
        if (tryDirtyTree && dirtyTreeMutex_.try_lock()) {
            if (dirtyTree_.TakeUnitsLowestAddr(totalNeeded, rawIdx, false)) {
                dirtyTreeMutex_.unlock();
                needClear = true;
                goto align_and_return;
            }
            tryDirtyTree = false;
            dirtyTreeMutex_.unlock();
        }
        if (tryReleasedTree && releasedTreeMutex_.try_lock()) {
            if (releasedTree_.TakeUnitsLowestAddr(totalNeeded, rawIdx, false)) {
                releasedTreeMutex_.unlock();
                needClear = false;
                goto align_and_return;
            }
            tryReleasedTree = false;
            releasedTreeMutex_.unlock();
        }
        ScopedEnterSaferegion enterSaferegion(true);
    }
    return false;

align_and_return:
    {
        UnitIndex alignedIdx = ((rawIdx + alignment - 1) / alignment) * alignment;
        // Return leading fragment
        if (alignedIdx > rawIdx) {
            ReclaimUnitsInternal(rawIdx, alignedIdx - rawIdx);
        }
        // Return trailing fragment
        UnitIndex rawEnd = rawIdx + totalNeeded;
        UnitIndex alignedEnd = alignedIdx + num;
        if (alignedEnd < rawEnd) {
            ReclaimUnitsInternal(alignedEnd, rawEnd - alignedEnd);
        }
        if (needClear) {
            ClearUnits(alignedIdx, num);
        }
        idx = alignedIdx;
        return true;
    }
}

// ============================================================================
// ReclaimUnitsInternal — return units to dirty tree
// ============================================================================

void MSRegionManager::ReclaimUnitsInternal(UnitIndex idx, UnitCount num)
{
    ScopedEnterSaferegion enterSaferegion(true);
    std::lock_guard<std::mutex> lg(dirtyTreeMutex_);
    if (UNLIKELY(!dirtyTree_.MergeInsert(idx, num, false))) {
        LOG(RTLOG_FATAL, "[MS-GC] MSRegionManager: failed to reclaim units [%u+%u, %u)",
            idx, num, idx + num);
    }
}

// ============================================================================
// ClearUnits — zero-fill memory at given unit range
// ============================================================================

void MSRegionManager::ClearUnits(UnitIndex idx, UnitCount num) const
{
    uintptr_t addr = GetUnitAddress(idx);
    size_t bytes = static_cast<size_t>(num) * unitSize_;
    memset(reinterpret_cast<void*>(addr), 0, bytes);
}

// ============================================================================
// TakeRegion — allocate one MS_REGION_SIZE region
// ============================================================================

MSRegionInfo* MSRegionManager::TakeRegion()
{
    // Heuristic GC trigger check
    if (!Heap::GetHeap().IsGcStarted()) {
        Collector& collector = Heap::GetHeap().GetCollector();
        size_t threshold = collector.GetGCStats().GetThreshold();
        size_t allocated = Heap::GetHeap().GetAllocator().AllocatedBytes();
        if (allocated >= threshold) {
            collector.RequestGC(GC_REASON_HEU, true);
        }
    }

    UnitIndex idx = 0;
    // Small region must be aligned to unitsPerMSRegion_ so that
    // regionIdx = idx / unitsPerMSRegion_ roundtrips correctly in ReclaimRegion.
    if (!TakeUnitsAligned(static_cast<UnitCount>(unitsPerMSRegion_),
                          static_cast<UnitCount>(unitsPerMSRegion_), idx)) {
        return nullptr;
    }

    // Compute region index: unitIdx / unitsPerMSRegion
    size_t regionIdx = static_cast<size_t>(idx) / unitsPerMSRegion_;
    MRT_ASSERT(regionIdx < maxRegionCount_, "region index out of bounds");

    MSRegionInfo* info = &array_[regionIdx];
    // Debug: exhaustive check on every TakeRegion return
    if (UNLIKELY(info->regionType != MSRegionType::MS_REGION_FREE)) {
        LOG(RTLOG_FATAL, "[MS-GC] TakeRegion: array_[%zu] NOT FREE! "
            "regionType=%d, allocPtr=0x%zx, regionEnd=0x%zx, allocBitmap=%p, "
            "sizeClass=%u, sweepState=%d, sweepGen=%u, "
            "nextIdx=%d, prevIdx=%d, reserved=0x%llx, "
            "unitIdx=%u, array_=%p, info=%p",
            regionIdx, (int)info->regionType, info->allocPtr, info->regionEnd,
            (void*)info->allocBitmap, (unsigned)info->sizeClass,
            (int)info->sweepState, info->sweepGen,
            info->nextRegionIdx, info->prevRegionIdx,
            (unsigned long long)info->reserved,
            idx, (void*)array_, (void*)info);
    }
    return info;
}

// ============================================================================
// TakeRegionForLarge — allocate N units for a large object
// ============================================================================

MSRegionInfo* MSRegionManager::TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart)
{
    // Heuristic GC trigger check
    if (!Heap::GetHeap().IsGcStarted()) {
        Collector& collector = Heap::GetHeap().GetCollector();
        size_t threshold = collector.GetGCStats().GetThreshold();
        size_t allocated = Heap::GetHeap().GetAllocator().AllocatedBytes();
        if (allocated >= threshold) {
            collector.RequestGC(GC_REASON_HEU, true);
        }
    }

    UnitIndex idx = 0;
    // Large objects must be both start-aligned AND size-aligned to MS Region
    // boundaries (unitsPerMSRegion_).  Otherwise:
    //   - Unaligned start: regionIdx = idx/unitsPerMSRegion_ maps to wrong entry.
    //   - Unaligned size:  trailing partial region is not covered by spanCount,
    //     leaving MSRegionInfo entries un-initialized → heap metadata corruption.
    UnitCount alignment = static_cast<UnitCount>(unitsPerMSRegion_);
    UnitCount alignedUnitCount = static_cast<UnitCount>(
        ((unitCount + unitsPerMSRegion_ - 1) / unitsPerMSRegion_) * unitsPerMSRegion_);
    if (!TakeUnitsAligned(alignedUnitCount, alignment, idx)) {
        regionStart = 0;
        return nullptr;
    }

    regionStart = GetUnitAddress(idx);

    // The base MSRegionInfo for large objects
    size_t regionIdx = static_cast<size_t>(idx) / unitsPerMSRegion_;
    MRT_ASSERT(regionIdx < maxRegionCount_, "region index out of bounds");

    return &array_[regionIdx];
}

// ============================================================================
// ReclaimRegion — reclaim a small/medium MS Region
// ============================================================================

void MSRegionManager::ReclaimRegion(MSRegionInfo* region)
{
    size_t regionIdx = static_cast<size_t>(region - array_);
    UnitIndex unitIdx = static_cast<UnitIndex>(regionIdx * unitsPerMSRegion_);
    UnitCount unitCount = static_cast<UnitCount>(unitsPerMSRegion_);

    // Debug: verify MSRegionInfo was properly Reset BEFORE returning units
    if (UNLIKELY(region->regionType != MSRegionType::MS_REGION_FREE)) {
        LOG(RTLOG_FATAL, "[MS-GC] ReclaimRegion: region NOT FREE before reclaim! "
            "regionType=%d, allocPtr=0x%zx, regionEnd=0x%zx, allocBitmap=%p, "
            "sizeClass=%u, sweepGen=%u, regionIdx=%zu, unitIdx=%u",
            (int)region->regionType, region->allocPtr, region->regionEnd,
            (void*)region->allocBitmap, (unsigned)region->sizeClass,
            region->sweepGen, regionIdx, unitIdx);
    }

    DLOG(REGION, "[MS-GC] ReclaimRegion: regionIdx=%zu, unitIdx=%u, unitCount=%u",
         regionIdx, unitIdx, unitCount);

    ReclaimUnitsInternal(unitIdx, unitCount);
}

// ============================================================================
// ReclaimRegionLarge — reclaim a large Region spanning regionBytes
// regionStart must be the actual allocation start address (saved before Reset).
// ============================================================================

void MSRegionManager::ReclaimRegionLarge(uintptr_t regionStart, size_t regionBytes)
{
    UnitIndex unitIdx = static_cast<UnitIndex>((regionStart - heapStart_) / unitSize_);
    UnitCount unitCount = static_cast<UnitCount>(
        (regionBytes + unitSize_ - 1) / unitSize_);

    DLOG(REGION, "[MS-GC] ReclaimRegionLarge: regionStart=0x%zx, unitIdx=%u, unitCount=%u, bytes=%zu",
         regionStart, unitIdx, unitCount, regionBytes);

    ReclaimUnitsInternal(unitIdx, unitCount);
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
