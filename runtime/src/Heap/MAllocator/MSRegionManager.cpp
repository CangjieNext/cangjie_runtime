// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSRegionManager.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "AllocUtil.h"
#include "Heap/MAllocator/MSBitmapArena.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSRegionSpace.h"
#include "CangjieRuntime.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"
#include "Collector/Collector.h"
#include "Collector/CollectorResources.h"

namespace MapleRuntime {

namespace {
constexpr size_t DEFAULT_MS_SCAVENGE_WAKE_BYTES = 4 * MS_REGION_SIZE;
}

// External: system page size (defined in RegionManager.cpp).
extern const size_t MRT_PAGE_SIZE;

MSRegionManager::~MSRegionManager()
{
    // array_ points into the mmap region — no free needed.
    array_ = nullptr;
    MSBitmapArena::Cleanup();
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
    // Cap at 512GB virtual reservation.
    static constexpr size_t MAX_VIRTUAL_HEAP = 512ULL * 1024 * 1024 * 1024;
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
    size_t maxVirtualRegionCount = maxHeapAligned / MS_REGION_SIZE;
    size_t maxVirtualUnitCount = maxHeapAligned / unitSize_;

    // --- Initial committed segment ---
    size_t committedHeapSize = RoundUp<size_t>(heapSize, MS_REGION_SIZE);
    size_t committedRegionCount = committedHeapSize / MS_REGION_SIZE;
    size_t committedUnitCount = committedHeapSize / unitSize_;

    // Segment size for future growth: clamp to [64MB, 512MB], aligned to MS_REGION_SIZE.
    static constexpr size_t MIN_SEGMENT_SIZE = 64ULL * 1024 * 1024;
    static constexpr size_t MAX_SEGMENT_SIZE = 512ULL * 1024 * 1024;
    size_t clampedSegmentSize = std::min(std::max(heapSize, MIN_SEGMENT_SIZE), MAX_SEGMENT_SIZE);
    size_t segmentSize = RoundUp<size_t>(clampedSegmentSize, MS_REGION_SIZE);

    // Memory layout: [MSRegionInfo[maxVirtualRegionCount] | Heap[maxHeapAligned]]
    //                ^mmapBase                            ^heapStart             ^maxHeapEnd
    array_ = reinterpret_cast<MSRegionInfo*>(mmapBase);
    heapStart_ = mmapBase + maxMetadataSize;
    uintptr_t committedEnd = heapStart_ + committedHeapSize;
    uintptr_t maxHeapEnd = heapStart_ + maxHeapAligned;

    // Zero-initialize MSRegionInfo entries for the initial committed segment only.
    // Pages beyond committedRegionCount are OS-zero (lazy commit) but we do NOT
    // touch them to avoid physical memory allocation.
    memset(array_, 0, committedRegionCount * sizeof(MSRegionInfo));
    for (size_t i = 0; i < committedRegionCount; i++) {
        array_[i].regionType = MSRegionType::MS_REGION_FREE;
        array_[i].nextRegionIdx = MSRegionInfo::INVALID_IDX;
        array_[i].prevRegionIdx = MSRegionInfo::INVALID_IDX;
    }

    freeManager_.Initialize(static_cast<UnitCount>(maxVirtualUnitCount), unitSize_, unitsPerMSRegion_,
                            static_cast<RegionCount>(committedRegionCount),
                            static_cast<RegionCount>(maxVirtualRegionCount),
                            heapStart_, committedEnd, maxHeapEnd, segmentSize);
    MSBitmapArena::Init(maxVirtualRegionCount, committedRegionCount);
    statsWindowStartNs_.store(TimeUtil::NanoSeconds(), std::memory_order_relaxed);

    VLOG(REPORT, "[MS-GC] MSRegionManager initialized (virtual reserve): mmapBase=0x%zx, "
        "maxMetadata=%zuKB, heapStart=0x%zx, committedEnd=0x%zx, maxHeapEnd=0x%zx, "
        "committedRegions=%zu, maxVirtualRegions=%zu, segmentSize=%zuMB, "
        "committedUnits=%zu, maxVirtualUnits=%zu, unitsPerMSRegion=%zu",
        mmapBase, maxMetadataSize / 1024,
        heapStart_, committedEnd, maxHeapEnd,
        committedRegionCount, maxVirtualRegionCount,
        segmentSize / (1024 * 1024),
        committedUnitCount, maxVirtualUnitCount, unitsPerMSRegion_);
}

void MSRegionManager::InitializeFreshRegions(RegionIndex startIdx, RegionCount count)
{
    size_t startRegionIdx = static_cast<size_t>(startIdx);
    size_t regionCount = static_cast<size_t>(count);
    MSBitmapArena::EnsureRegionRangeCommitted(startRegionIdx, regionCount);
    for (size_t i = 0; i < regionCount; ++i) {
        size_t regionIdx = startRegionIdx + i;
        array_[regionIdx].regionType = MSRegionType::MS_REGION_FREE;
        array_[regionIdx].nextRegionIdx = MSRegionInfo::INVALID_IDX;
        array_[regionIdx].prevRegionIdx = MSRegionInfo::INVALID_IDX;
    }
}

void MSRegionManager::OnCommittedHeapExtended(uintptr_t newCommittedEnd)
{
    Heap::OnHeapExtended(newCommittedEnd);
}

// ============================================================================
// O4: small-object HEU trigger checkpoint
// ============================================================================

namespace {
uint64_t ReadEnvTimeNs(const char* name, uint64_t fallback)
{
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return fallback;
    }
    uint64_t value = CString::ParseTimeFromEnv(env);
    return value == 0 ? fallback : value;
}

size_t ReadEnvCount(const char* name, size_t fallback)
{
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return fallback;
    }
    size_t value = CString::ParsePosNumFromEnv(env);
    return value == 0 ? fallback : value;
}

double ReadEnvPositiveDecimal(const char* name, double fallback)
{
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return fallback;
    }
    double value = CString::ParsePosDecFromEnv(env);
    return value == 0.0 ? fallback : value;
}

uint64_t GetHeuRequestCooldownNs()
{
    static const uint64_t value = ReadEnvTimeNs("CJ_MS_HEU_REQUEST_COOLDOWN", 10ULL * 1000ULL * 1000ULL);
    return value;
}

bool GetInlineReclaimEnabled()
{
    static const bool value = ReadEnvCount("CJ_MS_INLINE_RECLAIM", 1) != 0;
    return value;
}

// GetBgSweepMaxRegionsPerSlice — 后台批量处理 region 数（与 CentralRegionPool 共用同一环境变量）。
// When the HEU threshold is hit, the system is already under allocation pressure; we reuse the
// background-sweep slice limit so that sweep-first deferral uses the same knob as the bg-sweep path.
size_t GetBgSweepMaxRegionsPerSlice()
{
    static const size_t value = ReadEnvCount("CJ_MS_BG_SWEEP_MAX_REGIONS_PER_SLICE", 256);
    return value;
}

size_t GetReleasedBusyRetryLimit()
{
    static const size_t value = ReadEnvCount("CJ_MS_RELEASED_BUSY_RETRY_LIMIT", 8);
    return value;
}
}

void MSRegionManager::MaybeNotifySweep(const CentralRegionPool* centralPool, uint8_t cls)
{
    if (centralPool == nullptr) {
        return;
    }

    BackgroundProcessor& backgroundProcessor = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();

    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }

    CentralRegionPool::BucketPressure pressure = centralPool->GetBucketPressure(cls);
    if (pressure.UnsweptCount() > 0) {
        backgroundProcessor.NotifyToSweep(cls);
    }
}

size_t MSRegionManager::GetHotFreeBytes() const
{
    return freeManager_.GetHotFreeBytes();
}

void MSRegionManager::MaybeNotifyScavenge()
{
    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }

    Collector& collector = Heap::GetHeap().GetCollector();
    const GCStats& gcStats = collector.GetGCStats();
    size_t threshold = gcStats.GetThreshold();
    if (threshold == 0) {
        return;
    }

    const CentralRegionPool* pool =
        static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator()).GetCentralPool();
    if (pool == nullptr) {
        return;
    }

    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    size_t totalBytes = pool->GetTotalMSRegionBytes();
    size_t hotFree = freeManager_.GetHotFreeBytes();
    size_t largeCache = pool->GetLargeHotCacheBytes();
    size_t heapRetained = totalBytes + hotFree + largeCache;

    size_t emptySwept = space.GetEmptySweptBytes();
    size_t releasableSupply = hotFree + emptySwept + largeCache;

    // Do not wake BackgroundScavengeLoop when releasable supply is insufficient to
    // produce a meaningful slice *after* preserving the mark-window reserve buffer.
    // The guard is the larger of the reserve bytes and the minimum slice size.
    {
        size_t reserveBytes = gcStats.markWindowReserveBytes;
        size_t wakeGuard = std::max(reserveBytes,
                                    static_cast<size_t>(DEFAULT_MS_SCAVENGE_WAKE_BYTES));
        if (releasableSupply <= wakeGuard) {
            return;
        }
    }

    static constexpr double kHysteresis = 0.1;
    if (heapRetained <= static_cast<size_t>(static_cast<double>(threshold) * (1.0 + kHysteresis))) {
        return;
    }

    BackgroundProcessor& bp = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
    bp.NotifyToScavenge();
}

void MSRegionManager::MaybeNotifyMark()
{
    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }

    uint64_t nowNs = TimeUtil::NanoSeconds();
    uint64_t lastHeuRequestNs = lastHeuRequestNs_.load(std::memory_order_relaxed);
    if (lastHeuRequestNs != 0 && nowNs - lastHeuRequestNs < GetHeuRequestCooldownNs()) {
        return;
    }

    Collector& collector = Heap::GetHeap().GetCollector();
    const GCStats& gcStats = collector.GetGCStats();
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    const CentralRegionPool* pool =
        space.GetCentralPool();
    if (pool == nullptr) {
        return;
    }

    // Use heapThreshold updated at GC end from active-region bytes and eager-sweep sampling.
    // Subtract the mark-window reserve so GC triggers early enough to leave free regions
    // available throughout the next concurrent mark phase.
    size_t heapInUse = space.GetHeapInUseBytes();
    size_t threshold = gcStats.GetThreshold();
    {
        size_t reserveBytes = gcStats.markWindowReserveBytes;
        size_t effectiveThreshold = (threshold > reserveBytes) ? (threshold - reserveBytes) : 0;
        if (heapInUse < effectiveThreshold) {
            return;
        }
    }

    // Sweep-first deferral: if unswept backlog exceeds one sweep-slice worth of
    // regions, kick background sweep and skip this GC request.  Sweep will
    // convert unswept regions into ready/empty regions, potentially bringing
    // effectiveAlloc back below threshold and avoiding an unnecessary GC cycle.
    size_t totalUnswept = pool->GetTotalUnsweptCount();
    size_t sweepSlice = GetBgSweepMaxRegionsPerSlice();
    if (totalUnswept > sweepSlice) {
        BackgroundProcessor& bp =
            Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
        // Enter pressure mode: MaybeNotifyMark is the only owner of sweep pressure.
        // Background sweep will keep draining slices until unswept count reaches 0.
        bp.SetSweepPressureMode(true);
        bp.NotifyToSweep(kNoPreferredSweepClass);
        return;
    }

    if (!lastHeuRequestNs_.compare_exchange_strong(lastHeuRequestNs, nowNs,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
        return;
    }

    collector.RequestGC(GC_REASON_HEU, true);
    VLOG(REPORT,
         "[MS-GC] HEU trigger (take-regions): active-region bytes=%zuB heapThreshold=%zuB"
         " effectiveThreshold=%zuB reserveBytes=%zuB reason=GC_REASON_HEU",
         heapInUse,
         threshold,
         (threshold > gcStats.markWindowReserveBytes) ? (threshold - gcStats.markWindowReserveBytes) : 0,
         gcStats.markWindowReserveBytes);
}

// ============================================================================
// TakeRegions — allocate contiguous regions from the unified page-heap manager.
// ============================================================================

bool MSRegionManager::TakeRegions(RegionCount num, RegionIndex& idx, AllocSource* source,
                                  bool allowReleasedTree)
{
    (void)allowReleasedTree;
    RegionIndex rawIdx = 0;
    uint64_t startNs = TimeUtil::NanoSeconds();

    MaybeNotifyMark();

    while (true) {
        FreeManager::TryTakeResult result = freeManager_.TryTakeRegions(num, rawIdx);
        if (result == FreeManager::TryTakeResult::SUCCESS) {
            idx = rawIdx;
            if (source != nullptr) {
                *source = AllocSource::RELEASED;
            }
            allocFromReleasedCount_.fetch_add(1, std::memory_order_relaxed);
            takeRegionsCallCount_.fetch_add(1, std::memory_order_relaxed);
            takeRegionsTimeNs_.fetch_add(TimeUtil::NanoSeconds() - startNs, std::memory_order_relaxed);
            MaybeNotifyScavenge();
            return true;
        }

        if (!freeManager_.GrowOneSegment(*this)) {
            takeRegionsCallCount_.fetch_add(1, std::memory_order_relaxed);
            takeRegionsTimeNs_.fetch_add(TimeUtil::NanoSeconds() - startNs, std::memory_order_relaxed);
            return false;
        }
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

    uint64_t releasedHits = allocFromReleasedCount_.exchange(0, std::memory_order_relaxed);
    uint64_t bumpHits = allocFromBumpCount_.exchange(0, std::memory_order_relaxed);
    uint64_t clearCalls = clearUnitsCallCount_.exchange(0, std::memory_order_relaxed);
    uint64_t clearBytes = clearUnitsBytes_.exchange(0, std::memory_order_relaxed);
    uint64_t clearTimeNs = clearUnitsTimeNs_.exchange(0, std::memory_order_relaxed);

    uint64_t totalHits = releasedHits + bumpHits;
    uint64_t clearPct = (windowNs == 0) ? 0 : (clearTimeNs * 100 / windowNs);

    VLOG(REPORT,
        "[MS-GC] alloc-hit window: released=%llu, bump=%llu, total=%llu; "
         "clear: calls=%llu, bytes=%llu, time=%lluns (~%llu%% of window)",
         static_cast<unsigned long long>(releasedHits),
         static_cast<unsigned long long>(bumpHits),
         static_cast<unsigned long long>(totalHits),
         static_cast<unsigned long long>(clearCalls),
         static_cast<unsigned long long>(clearBytes),
         static_cast<unsigned long long>(clearTimeNs),
         static_cast<unsigned long long>(clearPct));
}

void MSRegionManager::DumpLifetimeStats() const
{
    uint64_t takeRegionsCalls = takeRegionsCallCount_.load(std::memory_order_relaxed);
    uint64_t takeRegionsTimeNs = takeRegionsTimeNs_.load(std::memory_order_relaxed);
    uint64_t takeRegionsAvgNs = (takeRegionsCalls == 0) ? 0 : (takeRegionsTimeNs / takeRegionsCalls);
    FreeManager::FinalStats stats = freeManager_.GetFinalStats();

    fprintf(stderr,
            "[MS-TAKEREGION] calls=%llu total_ns=%llu total_ms=%.3f avg_ns=%llu\n",
            static_cast<unsigned long long>(takeRegionsCalls),
            static_cast<unsigned long long>(takeRegionsTimeNs),
            static_cast<double>(takeRegionsTimeNs) / 1000000.0,
            static_cast<unsigned long long>(takeRegionsAvgNs));
    fprintf(stderr,
            "[MS-FREEMANAGER] grows=%llu committed_regions=%zu committed_bytes=%zu free_regions=%zu free_bytes=%zu "
            "hot_free_regions=%zu hot_free_bytes=%zu released_regions=%zu released_bytes=%zu used_regions=%zu "
            "used_bytes=%zu metadata_bytes=%zu max_free_run_regions=%u max_virtual_regions=%zu\n",
            static_cast<unsigned long long>(stats.heapGrowCount),
            stats.committedRegions,
            stats.committedBytes,
            stats.freeRegions,
            stats.freeBytes,
            stats.hotFreeRegions,
            stats.hotFreeBytes,
            stats.scavengedRegions,
            stats.scavengedBytes,
            stats.usedRegions,
            stats.usedBytes,
            stats.metadataBytes,
            stats.maxFreeRunRegions,
            freeManager_.GetMaxVirtualRegionCount());
}

extern "C" void DumpLifetimeStats_MSRegionManager()
{
    auto& allocator = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    MSRegionManager* regionManager = allocator.GetMSRegionManager();
    if (regionManager != nullptr) {
        regionManager->DumpLifetimeStats();
    }
}

// ============================================================================
// TakeRegion — allocate one MS_REGION_SIZE region
// ============================================================================

MSRegionInfo* MSRegionManager::TakeRegion(uint8_t cls, const CentralRegionPool& centralPool)
{
    RegionIndex regionIdx = 0;
    AllocSource source = AllocSource::BUMP;
    if (!TakeRegions(1, regionIdx, &source, false)) {
        return nullptr;
    }


    MRT_ASSERT(regionIdx < freeManager_.GetMaxVirtualRegionCount(), "region index out of bounds");

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

MSRegionInfo* MSRegionManager::TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart,
                                                   const CentralRegionPool* centralPool)
{
    // Convert unit count to region count (round up to full regions).
    RegionCount regionCount = static_cast<RegionCount>(
        (unitCount + unitsPerMSRegion_ - 1) / unitsPerMSRegion_);

    RegionIndex regionIdx = 0;
    AllocSource source = AllocSource::BUMP;
    if (!TakeRegions(regionCount, regionIdx, &source, true)) {
        regionStart = 0;
        return nullptr;
    }

    // Inline reclaim: when allocating a large object, synchronously release
    // regionCount empty-cache regions to offset the RSS growth from the new
    // large span (whether from BUMP or RELEASED, both will be touched and
    // consume physical pages).
    if (GetInlineReclaimEnabled() && centralPool != nullptr) {
        for (RegionCount i = 0; i < regionCount; ++i) {
            InlineReclaimForBalance(*centralPool);
        }
    }

    regionStart = RegionIdxToAddr(regionIdx);

    MRT_ASSERT(regionIdx < freeManager_.GetMaxVirtualRegionCount(), "region index out of bounds");

    return &array_[regionIdx];
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
    freeManager_.ReclaimRegionsReleased(regionIdx, regionCount);
}

void MSRegionManager::ReclaimEmptyRegionToHotFree(MSRegionInfo* region)
{
    size_t regionIdx = static_cast<size_t>(region - array_);
    CHECK_DETAIL(region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE,
                 "[MS-GC] ReclaimEmptyRegionToHotFree: bad regionType=%u idx=%zu",
                 static_cast<unsigned>(region->regionType), regionIdx);

    region->Reset();
    freeManager_.ReclaimRegionsReleased(static_cast<RegionIndex>(regionIdx), 1);
}

size_t MSRegionManager::ScavengeFreeRegions(bool releaseAll)
{
    return freeManager_.ScavengeFreeRegions(releaseAll);
}

size_t MSRegionManager::ScavengeFreeRegionsUpTo(size_t maxBytes)
{
    return freeManager_.ScavengeFreeRegionsUpTo(maxBytes);
}

// ============================================================================
// InlineReclaimForBalance — synchronously release one empty-cache region
// to offset a BUMP allocation, keeping committed bytes stable.
// ============================================================================

void MSRegionManager::InlineReclaimForBalance(const CentralRegionPool& pool)
{
    if (pool.GetEmptySweptRegionCount() == 0) {
        return;
    }

    CentralRegionPool& mutablePool = const_cast<CentralRegionPool&>(pool);
    MSRegionInfo* region = mutablePool.TakeEmptySweptRegionForReclaim();
    if (region == nullptr) {
        return;
    }

    ReclaimEmptyRegionToHotFree(region);
    mutablePool.DecrementTotalMSRegionBytes(MS_REGION_SIZE);
}

// ============================================================================
// ReleaseGarbageRegions — no-op for MSGC.
// Small regions are kept in CentralRegionPool empty cache and scavenged from
// there. Large regions are reclaimed eagerly to releasedTree.
// ============================================================================

size_t MSRegionManager::ReleaseGarbageRegions(size_t targetCachedSize)
{
    (void)targetCachedSize;
    return 0;
}

// ============================================================================
// GrowOneSegment — delegate heap growth to FreeManager.
// FreeManager updates the committed heap state and calls back into
// MSRegionManager to initialize new MSRegionInfo entries.
// ============================================================================

bool MSRegionManager::GrowOneSegment()
{
    return freeManager_.GrowOneSegment(*this);
}

// ============================================================================
// ShrinkHeap — delegate trailing-tail reclaim and committed-heap shrink to FreeManager.
// ============================================================================

void MSRegionManager::ShrinkHeap()
{
    size_t oldCommittedRegionCount = freeManager_.GetCommittedRegionCount();
    freeManager_.ShrinkHeap();
    size_t newCommittedRegionCount = freeManager_.GetCommittedRegionCount();
    if (newCommittedRegionCount < oldCommittedRegionCount) {
        MSBitmapArena::ShrinkToCommittedRegionCount(newCommittedRegionCount);
    }
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
