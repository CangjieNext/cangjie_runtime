// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MSRegionManager.h"

#include <algorithm>
#include <cstring>
#ifndef _WIN64
#include <sys/mman.h>
#endif
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/MemUtils.h"
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

size_t MSRegionManager::ComputeMaxVirtualHeapSize(size_t heapSize)
{
    // Cap at 4GB virtual reservation.
    static constexpr size_t MAX_VIRTUAL_HEAP = 4ULL * 1024 * 1024 * 1024;
    size_t maxVirtual = RoundUp<size_t>(heapSize, MS_REGION_SIZE);
    if (maxVirtual < MAX_VIRTUAL_HEAP) {
        maxVirtual = MAX_VIRTUAL_HEAP;
    }
    return maxVirtual;
}

size_t MSRegionManager::GetVirtualReserveSize(size_t maxVirtualHeapSize)
{
    size_t metadataSize = GetMetadataSize(maxVirtualHeapSize);
    size_t heapAligned = RoundUp<size_t>(maxVirtualHeapSize, MS_REGION_SIZE);
    return metadataSize + heapAligned;
}

// ============================================================================
// Initialize — virtual reservation + initial committed segment
// ============================================================================

void MSRegionManager::Initialize(size_t heapSize, uintptr_t mmapBase, size_t maxVirtualHeapSize)
{
    unitSize_ = MRT_PAGE_SIZE;
    unitsPerMSRegion_ = MS_REGION_SIZE / unitSize_;

    // --- Max virtual capacity (full mmap reservation) ---
    size_t maxMetadataSize = GetMetadataSize(maxVirtualHeapSize);
    size_t maxHeapAligned = RoundUp<size_t>(maxVirtualHeapSize, MS_REGION_SIZE);
    maxVirtualRegionCount_ = maxHeapAligned / MS_REGION_SIZE;
    maxVirtualUnitCount_ = maxHeapAligned / unitSize_;

    // --- Initial committed segment ---
    size_t committedHeapSize = RoundUp<size_t>(heapSize, MS_REGION_SIZE);
    committedRegionCount_ = committedHeapSize / MS_REGION_SIZE;
    committedUnitCount_ = committedHeapSize / unitSize_;

    // Segment size for future growth: max(heapSize, 64MB), aligned to MS_REGION_SIZE.
    static constexpr size_t MIN_SEGMENT_SIZE = 64ULL * 1024 * 1024;
    segmentSize_ = RoundUp<size_t>(std::max(heapSize, MIN_SEGMENT_SIZE), MS_REGION_SIZE);

    // Memory layout: [MSRegionInfo[maxVirtualRegionCount] | Heap[maxHeapAligned]]
    //                ^mmapBase                            ^heapStart             ^maxHeapEnd
    array_ = reinterpret_cast<MSRegionInfo*>(mmapBase);
    heapStart_ = mmapBase + maxMetadataSize;
    committedEnd_ = heapStart_ + committedHeapSize;
    maxHeapEnd_ = heapStart_ + maxHeapAligned;
    inactiveZone_.store(heapStart_, std::memory_order_relaxed);

    // Zero-initialize MSRegionInfo entries for the initial committed segment only.
    // Pages beyond committedRegionCount_ are OS-zero (lazy commit) but we do NOT
    // touch them to avoid physical memory allocation.
    memset(array_, 0, committedRegionCount_ * sizeof(MSRegionInfo));
    for (size_t i = 0; i < committedRegionCount_; i++) {
        array_[i].regionType = MSRegionType::MS_REGION_FREE;
        array_[i].nextRegionIdx = MSRegionInfo::INVALID_IDX;
        array_[i].prevRegionIdx = MSRegionInfo::INVALID_IDX;
    }

    // Initialize CartesianTrees with MAX VIRTUAL unit count so they can handle
    // indices from future growth segments.
    dirtyTree_.Init(maxVirtualUnitCount_);
    releasedTree_.Init(maxVirtualUnitCount_);

    VLOG(REPORT, "[MS-GC] MSRegionManager initialized (virtual reserve): mmapBase=0x%zx, "
        "maxMetadata=%zuKB, heapStart=0x%zx, committedEnd=0x%zx, maxHeapEnd=0x%zx, "
        "committedRegions=%zu, maxVirtualRegions=%zu, segmentSize=%zuMB, "
        "committedUnits=%zu, maxVirtualUnits=%zu, unitsPerMSRegion=%zu",
        mmapBase, maxMetadataSize / 1024,
        heapStart_, committedEnd_, maxHeapEnd_,
        committedRegionCount_, maxVirtualRegionCount_,
        segmentSize_ / (1024 * 1024),
        committedUnitCount_, maxVirtualUnitCount_, unitsPerMSRegion_);
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
    if (addr + byteCount <= committedEnd_) {
        idx = static_cast<UnitIndex>((addr - heapStart_) / unitSize_);
        return true;
    }
    // Rollback
    inactiveZone_.fetch_sub(byteCount, std::memory_order_relaxed);

    // 4. Try to grow the committed heap, then retry inactiveZone extension.
    if (GrowOneSegment()) {
        addr = inactiveZone_.fetch_add(byteCount, std::memory_order_relaxed);
        if (addr + byteCount <= committedEnd_) {
            idx = static_cast<UnitIndex>((addr - heapStart_) / unitSize_);
            return true;
        }
        inactiveZone_.fetch_sub(byteCount, std::memory_order_relaxed);
    }

    return false;
}

// ============================================================================
// TakeUnitsAligned — allocate units with alignment guarantee.
// Over-allocates from dirtyTree/releasedTree and returns unneeded fragments.
// Falls back to inactiveZone which is naturally aligned at initialization.
// ============================================================================

bool MSRegionManager::TakeUnitsAligned(UnitCount num, UnitCount alignment, UnitIndex& idx)
{
    // Priority 1 & 2: allocate from dirty/released trees with over-allocation.
    // Prefer reusing reclaimed memory to keep inactiveZone watermark low.
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

    // Priority 3: extend inactiveZone (bump allocator, fallback).
    {
        size_t byteCount = static_cast<size_t>(num) * unitSize_;
        // Align the bump pointer to alignment * unitSize_ boundary
        uintptr_t alignBytes = static_cast<uintptr_t>(alignment) * unitSize_;
        uintptr_t oldAddr = inactiveZone_.load(std::memory_order_relaxed);
        uintptr_t alignedAddr;
        do {
            alignedAddr = ((oldAddr + alignBytes - 1) / alignBytes) * alignBytes;
            if (alignedAddr + byteCount <= committedEnd_) {
                break;  // fits in committed region
            }
            // Try to grow the heap
            if (!GrowOneSegment()) {
                return false;  // at max capacity, OOM
            }
            // Reload after potential growth (committedEnd_ may have changed)
            oldAddr = inactiveZone_.load(std::memory_order_relaxed);
        } while (true);

        // CAS loop to atomically claim the aligned region
        oldAddr = inactiveZone_.load(std::memory_order_relaxed);
        do {
            alignedAddr = ((oldAddr + alignBytes - 1) / alignBytes) * alignBytes;
            if (alignedAddr + byteCount > committedEnd_) {
                // Another thread consumed the space; try grow again
                if (!GrowOneSegment()) {
                    return false;
                }
                oldAddr = inactiveZone_.load(std::memory_order_relaxed);
                continue;
            }
        } while (!inactiveZone_.compare_exchange_weak(oldAddr, alignedAddr + byteCount,
                                                       std::memory_order_relaxed));
        idx = static_cast<UnitIndex>((alignedAddr - heapStart_) / unitSize_);
        return true;
    }

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
    MRT_ASSERT(regionIdx < maxVirtualRegionCount_, "region index out of bounds");

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
    MRT_ASSERT(regionIdx < maxVirtualRegionCount_, "region index out of bounds");

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

    DLOG(REGION, "[MS-GC] ReclaimRegionLarge->releasedTree: regionStart=0x%zx, unitIdx=%u, unitCount=%u, bytes=%zuKB",
         regionStart, unitIdx, unitCount, regionBytes / 1024);

    // Large objects (>= MS_REGION_SIZE) release physical pages immediately
    // and go to releasedTree, avoiding dirty cache bloat.
    ReclaimUnitsReleased(unitIdx, unitCount);
}

// ============================================================================
// ReleaseUnits — platform-specific physical page release
// ============================================================================

void MSRegionManager::ReleaseUnits(UnitIndex idx, UnitCount num) const
{
    uintptr_t addr = GetUnitAddress(idx);
    size_t size = static_cast<size_t>(num) * unitSize_;
    DLOG(REGION, "[MS-GC] ReleaseUnits: idx=%u, num=%u, addr=0x%zx, size=%zu",
         idx, num, addr, size);
#if defined(_WIN64)
    CHECK_E(UNLIKELY(!VirtualFree(reinterpret_cast<void*>(addr), size, MEM_DECOMMIT)),
            "VirtualFree failed in MSRegionManager::ReleaseUnits");
#elif defined(__APPLE__)
    MemorySet(addr, size, 0, size);
    void* ret = mmap(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ret == MAP_FAILED) {
        LOG(RTLOG_ERROR, "[MS-GC] ReleaseUnits mmap fixed failed");
    }
#else
    (void)madvise(reinterpret_cast<void*>(addr), size, MADV_DONTNEED);
#endif
}

// ============================================================================
// ReclaimUnitsReleased — release physical pages then insert to released tree
// ============================================================================

void MSRegionManager::ReclaimUnitsReleased(UnitIndex idx, UnitCount num)
{
    ReleaseUnits(idx, num);
    ScopedEnterSaferegion enterSaferegion(true);
    std::lock_guard<std::mutex> lg(releasedTreeMutex_);
    if (UNLIKELY(!releasedTree_.MergeInsert(idx, num, false))) {
        LOG(RTLOG_FATAL, "[MS-GC] MSRegionManager: failed to reclaim released units [%u+%u, %u)",
            idx, num, idx + num);
    }
}

// ============================================================================
// ReleaseGarbageRegions — batch migrate dirty → released tree
// Keeps up to targetCachedSize bytes in dirtyTree as hot cache.
// ============================================================================

size_t MSRegionManager::ReleaseGarbageRegions(size_t targetCachedSize)
{
    size_t dirtyBytes = static_cast<size_t>(dirtyTree_.GetTotalCount()) * unitSize_;
    size_t releasedTreeBytes = static_cast<size_t>(releasedTree_.GetTotalCount()) * unitSize_;
    DLOG(REGION, "[MS-GC] ReleaseGarbageRegions enter: dirtyTree=%zuKB, releasedTree=%zuKB, target=%zuKB",
         dirtyBytes / 1024, releasedTreeBytes / 1024, targetCachedSize / 1024);
    if (dirtyBytes <= targetCachedSize) {
        return 0;
    }

    size_t releasedBytes = 0;
    while (dirtyBytes > targetCachedSize) {
        UnitIndex idx = 0;
        UnitCount num = 0;
        {
            ScopedEnterSaferegion enterSaferegion(true);
            std::lock_guard<std::mutex> lg(dirtyTreeMutex_);
            if (!dirtyTree_.RemoveRootNode(idx, num)) {
                break;
            }
        }

        // Release physical pages for extracted units.
        ReleaseUnits(idx, num);

        {
            ScopedEnterSaferegion enterSaferegion(true);
            std::lock_guard<std::mutex> lg(releasedTreeMutex_);
            if (UNLIKELY(!releasedTree_.MergeInsert(idx, num, false))) {
                LOG(RTLOG_FATAL, "[MS-GC] ReleaseGarbageRegions: failed to insert released units [%u+%u)",
                    idx, num);
            }
        }

        releasedBytes += static_cast<size_t>(num) * unitSize_;
        dirtyBytes = static_cast<size_t>(dirtyTree_.GetTotalCount()) * unitSize_;
    }

    DLOG(REGION, "[MS-GC] ReleaseGarbageRegions: dirty->released %zuKB, dirtyCache %zuKB (target %zuKB)",
         releasedBytes / 1024, dirtyBytes / 1024, targetCachedSize / 1024);
    return releasedBytes;
}

// ============================================================================
// GrowOneSegment — extend the committed heap by one segmentSize_ chunk.
// Initializes the MSRegionInfo entries for the new regions and updates
// committedEnd_ / committedRegionCount_ / committedUnitCount_.
// Also updates Heap::heapCurrentEnd so that IsHeapAddress covers the new space.
// ============================================================================

bool MSRegionManager::GrowOneSegment()
{
    std::lock_guard<std::mutex> lg(growMutex_);

    // Double-check after acquiring lock (another thread may have grown already).
    if (committedEnd_ >= maxHeapEnd_) {
        return false;  // Already at maximum virtual capacity.
    }

    uintptr_t oldCommittedEnd = committedEnd_;
    size_t growBytes = segmentSize_;
    // Don't exceed maxHeapEnd_.
    if (oldCommittedEnd + growBytes > maxHeapEnd_) {
        growBytes = maxHeapEnd_ - oldCommittedEnd;
    }

    size_t growRegions = growBytes / MS_REGION_SIZE;
    size_t growUnits = growBytes / unitSize_;
    size_t oldRegionCount = committedRegionCount_;

    // Initialize MSRegionInfo entries for the new regions.
    // On macOS/Linux with lazy commit, the underlying pages are zero-filled
    // on first access.  We still explicitly set the required fields.
    for (size_t i = 0; i < growRegions; i++) {
        size_t regionIdx = oldRegionCount + i;
        array_[regionIdx].regionType = MSRegionType::MS_REGION_FREE;
        array_[regionIdx].nextRegionIdx = MSRegionInfo::INVALID_IDX;
        array_[regionIdx].prevRegionIdx = MSRegionInfo::INVALID_IDX;
    }

    // Update committed boundaries.
    committedRegionCount_ += growRegions;
    committedUnitCount_ += growUnits;
    committedEnd_ = oldCommittedEnd + growBytes;

    // Update the global heap end so Heap::IsHeapAddress covers the new range.
    Heap::OnHeapExtended(committedEnd_);

    VLOG(REPORT, "[MS-GC] GrowOneSegment: +%zuMB \u2192 committedEnd=0x%zx, "
        "committedRegions=%zu/%zu, committedUnits=%zu/%zu",
        growBytes / (1024 * 1024), committedEnd_,
        committedRegionCount_, maxVirtualRegionCount_,
        committedUnitCount_, maxVirtualUnitCount_);

    return true;
}

// ============================================================================
// ShrinkHeap — reclaim trailing released units and shrink committed heap.
//
// Phase 1: Find releasedTree nodes at the tail (adjacent to inactiveZone_).
//          Remove them and CAS-lower inactiveZone_ to their start address.
//          This effectively "un-allocates" released memory at the heap top.
//
// Phase 2: If the virgin gap (inactiveZone_ to committedEnd_) covers at
//          least one full segment, shrink committedEnd_ downward.
//          Does NOT update Heap::heapCurrentEnd (keeps peak for IsHeapAddress).
// ============================================================================

void MSRegionManager::ShrinkHeap()
{
    std::lock_guard<std::mutex> lg(growMutex_);

    uintptr_t originalIz = inactiveZone_.load(std::memory_order_relaxed);
    size_t gap = (committedEnd_ > originalIz) ? (committedEnd_ - originalIz) : 0;
    uint32_t dirtyUnits = dirtyTree_.GetTotalCount();
    uint32_t releasedUnits = releasedTree_.GetTotalCount();
    VLOG(REPORT, "[MS-GC] ShrinkHeap enter: inactiveZone=0x%zx, committedEnd=0x%zx, "
         "gap=%zuKB, dirty=%u units, released=%u units, segmentSize=%zuMB",
         originalIz, committedEnd_, gap / 1024, dirtyUnits, releasedUnits,
         segmentSize_ / (1024 * 1024));

    // Phase 1: Reclaim trailing released nodes contiguous with inactiveZone_.
    size_t reclaimedUnits = 0;
    while (true) {
        uintptr_t iz = inactiveZone_.load(std::memory_order_relaxed);
        if (iz <= heapStart_) {
            break;
        }
        UnitIndex izUnitIdx = static_cast<UnitIndex>((iz - heapStart_) / unitSize_);

        UnitIndex nodeIdx = 0;
        UnitCount nodeNum = 0;
        bool found = false;
        {
            std::lock_guard<std::mutex> rlg(releasedTreeMutex_);
            found = releasedTree_.TakeRightmostIfEndEquals(izUnitIdx, nodeIdx, nodeNum);
        }

        if (!found) {
            break;
        }

        uintptr_t newIz = GetUnitAddress(nodeIdx);
        if (!inactiveZone_.compare_exchange_strong(iz, newIz, std::memory_order_relaxed)) {
            // Concurrent allocator advanced inactiveZone_. Restore node.
            std::lock_guard<std::mutex> rlg(releasedTreeMutex_);
            releasedTree_.MergeInsert(nodeIdx, nodeNum, false);
            break;
        }
        reclaimedUnits += nodeNum;
    }

    if (reclaimedUnits > 0) {
        uintptr_t newIz = inactiveZone_.load(std::memory_order_relaxed);
        VLOG(REPORT, "[MS-GC] ShrinkHeap phase1: reclaimed %zu trailing released units, "
             "inactiveZone 0x%zx -> 0x%zx", reclaimedUnits, originalIz, newIz);
    }

    // Phase 2: Shrink committedEnd_ by whole segments above inactiveZone_.
    uintptr_t iz = inactiveZone_.load(std::memory_order_relaxed);
    if (committedEnd_ <= iz) {
        return;
    }

    size_t shrinkGap = committedEnd_ - iz;
    size_t shrinkSegments = shrinkGap / segmentSize_;
    if (shrinkSegments == 0) {
        return;
    }

    size_t shrinkBytes = shrinkSegments * segmentSize_;
    uintptr_t newCommitted = committedEnd_ - shrinkBytes;

    // Don't shrink below one segment from heapStart_.
    if (newCommitted < heapStart_ + segmentSize_) {
        newCommitted = heapStart_ + segmentSize_;
        if (newCommitted >= committedEnd_) {
            return;
        }
        shrinkBytes = committedEnd_ - newCommitted;
    }

    uintptr_t oldCommitted = committedEnd_;
    committedEnd_ = newCommitted;
    committedRegionCount_ = (newCommitted - heapStart_) / MS_REGION_SIZE;
    committedUnitCount_ = (newCommitted - heapStart_) / unitSize_;

    // NOTE: Do NOT call Heap::OnHeapExtended(committedEnd_).
    // heapCurrentEnd must stay at peak so IsHeapAddress covers all ever-allocated addresses.

    VLOG(REPORT, "[MS-GC] ShrinkHeap phase2: -%zuMB \u2192 committedEnd=0x%zx (was 0x%zx), "
        "committedRegions=%zu, committedUnits=%zu, inactiveZone=0x%zx",
        shrinkBytes / (1024 * 1024), committedEnd_, oldCommitted,
        committedRegionCount_, committedUnitCount_, iz);
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
