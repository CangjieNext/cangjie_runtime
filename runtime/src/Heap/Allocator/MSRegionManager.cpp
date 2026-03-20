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
#include "Base/TimeUtils.h"
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

    // Initialize CartesianTrees with MAX VIRTUAL REGION count so they can handle
    // indices from future growth segments.  Tree Index/Count = region index/count.
    dirtyTree_.Init(maxVirtualRegionCount_);
    releasedTree_.Init(maxVirtualRegionCount_);
    statsWindowStartNs_.store(TimeUtil::NanoSeconds(), std::memory_order_relaxed);

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
// O4: unified HEU trigger checkpoint
// ============================================================================

void MSRegionManager::MaybeTriggerHeuGC()
{
    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }

    Collector& collector = Heap::GetHeap().GetCollector();
    const GCStats& gcStats = collector.GetGCStats();
    size_t threshold = gcStats.GetThreshold();
    size_t allocated = Heap::GetHeap().GetAllocator().AllocatedBytes();

    // O1 formula: effective allocation compensates for lazy-sweep lag.
    size_t baseline = gcStats.baselineAllocatedBytes;
    size_t newAlloc = (allocated > baseline) ? (allocated - baseline) : 0;
    size_t effectiveAlloc = gcStats.lastTrueLiveBytes + newAlloc;

    if (effectiveAlloc >= threshold) {
        collector.RequestGC(GC_REASON_HEU, true);  // async, non-blocking
    }
}

// ============================================================================
// TakeRegions — allocate contiguous regions from dirty/released tree or bump.
// Trees operate at region granularity: no over-allocation or alignment splitting.
// inactiveZone_ is always MS_REGION_SIZE-aligned (guaranteed by heapStart_
// alignment and region-granular increments), so bump needs no alignment step.
// ============================================================================

bool MSRegionManager::TakeRegions(RegionCount num, RegionIndex& idx)
{
    // Unified HEU trigger point for both small and large fresh-region paths.
    MaybeTriggerHeuGC();

    RegionIndex rawIdx = 0;
    bool tryDirtyTree = true;
    bool tryReleasedTree = true;

    while (tryDirtyTree || tryReleasedTree) {
        // refreshRegionInfo=false: MS mode does not use CopyGC's UnitInfo mirror.
        if (tryDirtyTree && dirtyTreeMutex_.try_lock()) {
            if (dirtyTree_.TakeUnitsLowestAddr(num, rawIdx, false)) {
                dirtyTreeMutex_.unlock();
                idx = rawIdx;
                allocFromDirtyCount_.fetch_add(1, std::memory_order_relaxed);
                ClearUnits(RegionToUnitIdx(idx), RegionCountToUnitCount(num));
                return true;
            }
            tryDirtyTree = false;
            dirtyTreeMutex_.unlock();
        }
        if (tryReleasedTree && releasedTreeMutex_.try_lock()) {
            if (releasedTree_.TakeUnitsLowestAddr(num, rawIdx, false)) {
                releasedTreeMutex_.unlock();
                idx = rawIdx;
                allocFromReleasedCount_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            tryReleasedTree = false;
            releasedTreeMutex_.unlock();
        }
        ScopedEnterSaferegion enterSaferegion(true);
    }

    // Fallback: extend inactiveZone (bump allocator).
    // inactiveZone_ is always MS_REGION_SIZE-aligned, so no alignment gap.
    {
        size_t byteCount = static_cast<size_t>(num) * MS_REGION_SIZE;
        uintptr_t oldAddr = inactiveZone_.load(std::memory_order_relaxed);
        do {
            if (oldAddr + byteCount <= committedEnd_) {
                break;  // fits in committed region
            }
            if (!GrowOneSegment()) {
                return false;  // at max capacity, OOM
            }
            oldAddr = inactiveZone_.load(std::memory_order_relaxed);
        } while (true);

        // CAS loop to atomically claim the region range
        oldAddr = inactiveZone_.load(std::memory_order_relaxed);
        do {
            if (oldAddr + byteCount > committedEnd_) {
                if (!GrowOneSegment()) {
                    return false;
                }
                oldAddr = inactiveZone_.load(std::memory_order_relaxed);
                continue;
            }
        } while (!inactiveZone_.compare_exchange_weak(oldAddr, oldAddr + byteCount,
                                                       std::memory_order_relaxed));
        idx = AddrToRegionIdx(oldAddr);
        allocFromBumpCount_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
}

// ============================================================================
// ReclaimRegionsDirty — return regions to dirty tree
// ============================================================================

void MSRegionManager::ReclaimRegionsDirty(RegionIndex idx, RegionCount num)
{
    ScopedEnterSaferegion enterSaferegion(true);
    std::lock_guard<std::mutex> lg(dirtyTreeMutex_);
    if (UNLIKELY(!dirtyTree_.MergeInsert(idx, num, false))) {
        LOG(RTLOG_FATAL, "[MS-GC] MSRegionManager: failed to reclaim dirty regions [%u+%u, %u)",
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
    uint64_t startNs = TimeUtil::NanoSeconds();
    memset(reinterpret_cast<void*>(addr), 0, bytes);
    uint64_t endNs = TimeUtil::NanoSeconds();
    const_cast<MSRegionManager*>(this)->clearUnitsCallCount_.fetch_add(1, std::memory_order_relaxed);
    const_cast<MSRegionManager*>(this)->clearUnitsBytes_.fetch_add(bytes, std::memory_order_relaxed);
    const_cast<MSRegionManager*>(this)->clearUnitsTimeNs_.fetch_add(endNs - startNs, std::memory_order_relaxed);
}

void MSRegionManager::DumpAndResetAllocClearStats()
{
    uint64_t nowNs = TimeUtil::NanoSeconds();
    uint64_t windowStartNs = statsWindowStartNs_.exchange(nowNs, std::memory_order_relaxed);
    uint64_t windowNs = (nowNs > windowStartNs) ? (nowNs - windowStartNs) : 0;

    uint64_t dirtyHits = allocFromDirtyCount_.exchange(0, std::memory_order_relaxed);
    uint64_t releasedHits = allocFromReleasedCount_.exchange(0, std::memory_order_relaxed);
    uint64_t bumpHits = allocFromBumpCount_.exchange(0, std::memory_order_relaxed);
    uint64_t clearCalls = clearUnitsCallCount_.exchange(0, std::memory_order_relaxed);
    uint64_t clearBytes = clearUnitsBytes_.exchange(0, std::memory_order_relaxed);
    uint64_t clearTimeNs = clearUnitsTimeNs_.exchange(0, std::memory_order_relaxed);

    uint64_t totalHits = dirtyHits + releasedHits + bumpHits;
    uint64_t clearPct = (windowNs == 0) ? 0 : (clearTimeNs * 100 / windowNs);

    VLOG(REPORT,
         "[MS-GC][O6] alloc-hit window: dirty=%llu, released=%llu, bump=%llu, total=%llu; "
         "clear: calls=%llu, bytes=%llu, time=%lluns (~%llu%% of window)",
         static_cast<unsigned long long>(dirtyHits),
         static_cast<unsigned long long>(releasedHits),
         static_cast<unsigned long long>(bumpHits),
         static_cast<unsigned long long>(totalHits),
         static_cast<unsigned long long>(clearCalls),
         static_cast<unsigned long long>(clearBytes),
         static_cast<unsigned long long>(clearTimeNs),
         static_cast<unsigned long long>(clearPct));
}

// ============================================================================
// TakeRegion — allocate one MS_REGION_SIZE region
// ============================================================================

MSRegionInfo* MSRegionManager::TakeRegion()
{
    // HEU trigger is handled at TakeRegions() entry.

    RegionIndex regionIdx = 0;
    if (!TakeRegions(1, regionIdx)) {
        return nullptr;
    }

    MRT_ASSERT(regionIdx < maxVirtualRegionCount_, "region index out of bounds");

    MSRegionInfo* info = &array_[regionIdx];
    // Debug: exhaustive check on every TakeRegion return
    if (UNLIKELY(info->regionType != MSRegionType::MS_REGION_FREE)) {
        LOG(RTLOG_FATAL, "[MS-GC] TakeRegion: array_[%u] NOT FREE! "
            "regionType=%d, allocPtr=0x%zx, regionEnd=0x%zx, allocBitmap=%p, "
            "sizeClass=%u, sweepState=%d, sweepGen=%u, "
            "nextIdx=%d, prevIdx=%d, reserved=0x%llx, "
            "array_=%p, info=%p",
            regionIdx, (int)info->regionType, info->allocPtr, info->regionEnd,
            (void*)info->allocBitmap, (unsigned)info->sizeClass,
            (int)info->sweepState, info->sweepGen,
            info->nextRegionIdx, info->prevRegionIdx,
            (unsigned long long)info->reserved,
            (void*)array_, (void*)info);
    }
    return info;
}

// ============================================================================
// TakeRegionForLarge — allocate N units for a large object
// ============================================================================

MSRegionInfo* MSRegionManager::TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart)
{
    // HEU trigger is handled at TakeRegions() entry.

    // Convert unit count to region count (round up to full regions).
    RegionCount regionCount = static_cast<RegionCount>(
        (unitCount + unitsPerMSRegion_ - 1) / unitsPerMSRegion_);

    RegionIndex regionIdx = 0;
    if (!TakeRegions(regionCount, regionIdx)) {
        regionStart = 0;
        return nullptr;
    }

    regionStart = RegionIdxToAddr(regionIdx);

    MRT_ASSERT(regionIdx < maxVirtualRegionCount_, "region index out of bounds");

    return &array_[regionIdx];
}

// ============================================================================
// ReclaimRegion — reclaim a small/medium MS Region
// ============================================================================

void MSRegionManager::ReclaimRegion(MSRegionInfo* region)
{
    size_t regionIdx = static_cast<size_t>(region - array_);

    // Debug: verify MSRegionInfo was properly Reset BEFORE returning
    if (UNLIKELY(region->regionType != MSRegionType::MS_REGION_FREE)) {
        LOG(RTLOG_FATAL, "[MS-GC] ReclaimRegion: region NOT FREE before reclaim! "
            "regionType=%d, allocPtr=0x%zx, regionEnd=0x%zx, allocBitmap=%p, "
            "sizeClass=%u, sweepGen=%u, regionIdx=%zu",
            (int)region->regionType, region->allocPtr, region->regionEnd,
            (void*)region->allocBitmap, (unsigned)region->sizeClass,
            region->sweepGen, regionIdx);
    }

    DLOG(REGION, "[MS-GC] ReclaimRegion: regionIdx=%zu", regionIdx);

    ReclaimRegionsDirty(static_cast<RegionIndex>(regionIdx), 1);
}

// ============================================================================
// ReclaimRegionLarge — reclaim a large Region spanning regionBytes
// regionStart must be the actual allocation start address (saved before Reset).
// ============================================================================

void MSRegionManager::ReclaimRegionLarge(uintptr_t regionStart, size_t regionBytes)
{
    RegionIndex regionIdx = AddrToRegionIdx(regionStart);
    RegionCount regionCount = static_cast<RegionCount>(
        (regionBytes + MS_REGION_SIZE - 1) / MS_REGION_SIZE);

    DLOG(REGION, "[MS-GC] ReclaimRegionLarge->releasedTree: regionStart=0x%zx, regionIdx=%u, regionCount=%u, bytes=%zuKB",
         regionStart, regionIdx, regionCount, regionBytes / 1024);

    // Large objects (>= MS_REGION_SIZE) release physical pages immediately
    // and go to releasedTree, avoiding dirty cache bloat.
    ReclaimRegionsReleased(regionIdx, regionCount);
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
// ReclaimRegionsReleased — release physical pages then insert to released tree
// ============================================================================

void MSRegionManager::ReclaimRegionsReleased(RegionIndex idx, RegionCount num)
{
    // Convert to unit range for physical page release.
    ReleaseUnits(RegionToUnitIdx(idx), RegionCountToUnitCount(num));
    ScopedEnterSaferegion enterSaferegion(true);
    std::lock_guard<std::mutex> lg(releasedTreeMutex_);
    if (UNLIKELY(!releasedTree_.MergeInsert(idx, num, false))) {
        LOG(RTLOG_FATAL, "[MS-GC] MSRegionManager: failed to reclaim released regions [%u+%u, %u)",
            idx, num, idx + num);
    }
}

// ============================================================================
// ReleaseGarbageRegions — batch migrate dirty → released tree
// Keeps up to targetCachedSize bytes in dirtyTree as hot cache.
// ============================================================================

size_t MSRegionManager::ReleaseGarbageRegions(size_t targetCachedSize)
{
    // Tree counts are region counts; convert to bytes for comparison.
    size_t dirtyBytes = static_cast<size_t>(dirtyTree_.GetTotalCount()) * MS_REGION_SIZE;
    size_t releasedTreeBytes = static_cast<size_t>(releasedTree_.GetTotalCount()) * MS_REGION_SIZE;
    DLOG(REGION, "[MS-GC] ReleaseGarbageRegions enter: dirtyTree=%zuKB, releasedTree=%zuKB, target=%zuKB",
         dirtyBytes / 1024, releasedTreeBytes / 1024, targetCachedSize / 1024);
    if (dirtyBytes <= targetCachedSize) {
        return 0;
    }

    size_t releasedBytes = 0;
    while (dirtyBytes > targetCachedSize) {
        RegionIndex regionIdx = 0;
        RegionCount regionCount = 0;
        {
            ScopedEnterSaferegion enterSaferegion(true);
            std::lock_guard<std::mutex> lg(dirtyTreeMutex_);
            if (!dirtyTree_.RemoveRootNode(regionIdx, regionCount)) {
                break;
            }
        }

        // Release physical pages (convert to unit range).
        ReleaseUnits(RegionToUnitIdx(regionIdx), RegionCountToUnitCount(regionCount));

        {
            ScopedEnterSaferegion enterSaferegion(true);
            std::lock_guard<std::mutex> lg(releasedTreeMutex_);
            if (UNLIKELY(!releasedTree_.MergeInsert(regionIdx, regionCount, false))) {
                LOG(RTLOG_FATAL, "[MS-GC] ReleaseGarbageRegions: failed to insert released regions [%u+%u)",
                    regionIdx, regionCount);
            }
        }

        releasedBytes += static_cast<size_t>(regionCount) * MS_REGION_SIZE;
        dirtyBytes = static_cast<size_t>(dirtyTree_.GetTotalCount()) * MS_REGION_SIZE;
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
    uint32_t dirtyRegions = dirtyTree_.GetTotalCount();
    uint32_t releasedRegions = releasedTree_.GetTotalCount();
    VLOG(REPORT, "[MS-GC] ShrinkHeap enter: inactiveZone=0x%zx, committedEnd=0x%zx, "
         "gap=%zuKB, dirty=%u regions, released=%u regions, segmentSize=%zuMB",
         originalIz, committedEnd_, gap / 1024, dirtyRegions, releasedRegions,
         segmentSize_ / (1024 * 1024));

    // Phase 1: Reclaim trailing released nodes contiguous with inactiveZone_.
    size_t reclaimedRegions = 0;
    while (true) {
        uintptr_t iz = inactiveZone_.load(std::memory_order_relaxed);
        if (iz <= heapStart_) {
            break;
        }
        RegionIndex izRegionIdx = AddrToRegionIdx(iz);

        RegionIndex nodeIdx = 0;
        RegionCount nodeNum = 0;
        bool found = false;
        {
            std::lock_guard<std::mutex> rlg(releasedTreeMutex_);
            found = releasedTree_.TakeRightmostIfEndEquals(izRegionIdx, nodeIdx, nodeNum);
        }

        if (!found) {
            break;
        }

        uintptr_t newIz = RegionIdxToAddr(nodeIdx);
        if (!inactiveZone_.compare_exchange_strong(iz, newIz, std::memory_order_relaxed)) {
            // Concurrent allocator advanced inactiveZone_. Restore node.
            std::lock_guard<std::mutex> rlg(releasedTreeMutex_);
            releasedTree_.MergeInsert(nodeIdx, nodeNum, false);
            break;
        }
        reclaimedRegions += nodeNum;
    }

    if (reclaimedRegions > 0) {
        uintptr_t newIz = inactiveZone_.load(std::memory_order_relaxed);
        VLOG(REPORT, "[MS-GC] ShrinkHeap phase1: reclaimed %zu trailing released regions, "
             "inactiveZone 0x%zx -> 0x%zx", reclaimedRegions, originalIz, newIz);
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
