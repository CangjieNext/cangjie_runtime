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
#include "Concurrency/ConcurrencyModel.h"
#include "CangjieRuntime.h"
#include "Base/SysCall.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"
#include "Collector/Collector.h"
#include "Collector/CollectorResources.h"
#include "Mutator/ThreadLocal.h"
#include "Heap/MAllocator/MSAllocBuffer.h"
#include "schedule.h"
namespace MapleRuntime {

namespace {
bool TryRoundUpSize(size_t value, size_t alignment, size_t& rounded)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return false;
    }
    size_t mask = alignment - 1;
    if (value > std::numeric_limits<size_t>::max() - mask) {
        return false;
    }
    rounded = (value + mask) & ~mask;
    return true;
}

bool IsSegmentRegionTypeInUse(MSRegionType type)
{
    switch (type) {
        case MSRegionType::MS_REGION_BUMP:
        case MSRegionType::MS_REGION_BITMAP:
        case MSRegionType::MS_REGION_PARTIAL:
        case MSRegionType::MS_REGION_FULL:
        case MSRegionType::MS_REGION_LARGE:
        case MSRegionType::MS_REGION_SPAN:
            return true;
        case MSRegionType::MS_REGION_FREE:
        case MSRegionType::MS_REGION_EMPTY_CACHE:
            return false;
    }
    return true;
}

} // namespace

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
    size_t heapAligned = 0;
    CHECK_DETAIL(TryRoundUpSize(heapSize, MS_REGION_SIZE, heapAligned),
                 "[MS-GC] heap size cannot be Region-aligned safely: %zu", heapSize);
    return heapAligned / MS_REGION_SIZE;
}

size_t MSRegionManager::GetMetadataSize(size_t heapSize)
{
    size_t heapAligned = 0;
    size_t metadataSize = 0;
    size_t totalSize = 0;
    CHECK_DETAIL(TryComputeVirtualReserveLayout(heapSize, heapAligned, metadataSize, totalSize),
                 "[MS-GC] virtual reserve layout overflows: heapSize=%zu", heapSize);
    (void)heapAligned;
    (void)totalSize;
    return metadataSize;
}

bool MSRegionManager::TryComputeVirtualReserveLayout(size_t heapSize,
                                                     size_t& heapAligned,
                                                     size_t& metadataSize,
                                                     size_t& totalSize)
{
    if (!TryRoundUpSize(heapSize, MS_REGION_SIZE, heapAligned)) {
        return false;
    }
    size_t regionCount = heapAligned / MS_REGION_SIZE;
    if (MRT_PAGE_SIZE == 0 || MS_REGION_SIZE % MRT_PAGE_SIZE != 0) {
        return false;
    }
    size_t unitsPerRegion = MS_REGION_SIZE / MRT_PAGE_SIZE;
    if (regionCount > std::numeric_limits<RegionCount>::max() ||
        unitsPerRegion == 0 ||
        regionCount > std::numeric_limits<UnitCount>::max() / unitsPerRegion ||
        regionCount > std::numeric_limits<size_t>::max() / sizeof(MSRegionInfo)) {
        return false;
    }
    size_t rawSize = regionCount * sizeof(MSRegionInfo);
    if (!TryRoundUpSize(rawSize, MRT_PAGE_SIZE, metadataSize) ||
        heapAligned > std::numeric_limits<size_t>::max() - metadataSize) {
        return false;
    }
    totalSize = metadataSize + heapAligned;
    return true;
}

size_t MSRegionManager::GetHeapMemorySize(size_t heapSize)
{
    size_t heapAligned = 0;
    size_t metadataSize = 0;
    size_t totalSize = 0;
    CHECK_DETAIL(TryComputeVirtualReserveLayout(heapSize, heapAligned, metadataSize, totalSize),
                 "[MS-GC] heap memory layout overflows: heapSize=%zu", heapSize);
    return totalSize;
}

size_t MSRegionManager::ComputeMaxVirtualHeapSize(size_t heapSize, size_t requestedVirtualHeapSize)
{
    size_t minVirtualHeapSize = 0;
    size_t configuredVirtualHeapSize = 0;
    CHECK_DETAIL(TryRoundUpSize(heapSize, MS_REGION_SIZE, minVirtualHeapSize) &&
                     TryRoundUpSize(requestedVirtualHeapSize, MS_REGION_SIZE, configuredVirtualHeapSize),
                 "[MS-GC] max virtual heap alignment overflows: initial=%zu configured=%zu",
                 heapSize, requestedVirtualHeapSize);
    return std::max(minVirtualHeapSize, configuredVirtualHeapSize);
}

size_t MSRegionManager::GetVirtualReserveSize(size_t maxVirtualHeapSize)
{
    size_t heapAligned = 0;
    size_t metadataSize = 0;
    size_t totalSize = 0;
    CHECK_DETAIL(TryComputeVirtualReserveLayout(
                     maxVirtualHeapSize, heapAligned, metadataSize, totalSize),
                 "[MS-GC] virtual reserve layout overflows: maxVirtual=%zu", maxVirtualHeapSize);
    return totalSize;
}

// ============================================================================
// Initialize — virtual reservation + initial committed segment
// ============================================================================

void MSRegionManager::Initialize(uintptr_t mmapBase, size_t maxVirtualHeapSize)
{
    unitSize_ = MRT_PAGE_SIZE;
    unitsPerMSRegion_ = MS_REGION_SIZE / unitSize_;
    MSAllocatorScaleConfig scaleConfig = CangjieRuntime::GetMSAllocatorScaleConfig();
    uint32_t shardCount = scaleConfig.shardCount;

    // --- Max virtual capacity (full mmap reservation) ---
    size_t maxHeapAligned = 0;
    size_t maxMetadataSize = 0;
    size_t totalReserveSize = 0;
    CHECK_DETAIL(TryComputeVirtualReserveLayout(
                     maxVirtualHeapSize, maxHeapAligned, maxMetadataSize, totalReserveSize),
                 "[MS-GC] virtual reserve layout overflows during manager initialization: maxVirtual=%zu",
                 maxVirtualHeapSize);
    (void)totalReserveSize;
    size_t maxVirtualRegionCount = maxHeapAligned / MS_REGION_SIZE;
    size_t maxVirtualUnitCount = maxHeapAligned / unitSize_;

    // --- Initial committed heap: independently configured 64MB segments ---
    size_t segmentSize = RoundUp<size_t>(kMSShardSegmentBytes, MS_REGION_SIZE);
    size_t initialCommittedHeapBytes = 0;
    CHECK_DETAIL(TryComputeMSInitialCommittedHeapBytes(
                     scaleConfig.initialSegmentCount, initialCommittedHeapBytes),
                 "[MS-GC] initial Segment count exceeds addressable heap: %zu",
                 scaleConfig.initialSegmentCount);
    size_t committedHeapSize = RoundUp<size_t>(initialCommittedHeapBytes, MS_REGION_SIZE);
    CHECK_DETAIL(committedHeapSize <= maxHeapAligned,
                 "[MS-GC] initial committed heap exceeds max virtual heap: committed=%zu maxVirtual=%zu "
                 "initialSegmentCount=%zu",
                 committedHeapSize, maxHeapAligned, scaleConfig.initialSegmentCount);
    size_t committedRegionCount = committedHeapSize / MS_REGION_SIZE;
    size_t committedUnitCount = committedHeapSize / unitSize_;

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
        array_[i].Reset();
    }

    freeManager_.Initialize(unitSize_, unitsPerMSRegion_, static_cast<RegionCount>(committedRegionCount),
                            static_cast<RegionCount>(maxVirtualRegionCount),
                            heapStart_, committedEnd, maxHeapEnd, segmentSize, shardCount);
    MSBitmapArena::Init(maxVirtualRegionCount);

    VLOG(REPORT, "[MS-GC] MSRegionManager initialized (virtual reserve): mmapBase=0x%zx, "
        "maxMetadata=%zuKB, heapStart=0x%zx, committedEnd=0x%zx, maxHeapEnd=0x%zx, "
        "committedRegions=%zu, maxVirtualRegions=%zu, shardCount=%u, processorsPerShard=%u, "
        "initialSegmentCount=%zu, segmentSize=%zuMB, "
        "committedUnits=%zu, maxVirtualUnits=%zu, unitsPerMSRegion=%zu",
        mmapBase, maxMetadataSize / 1024,
        heapStart_, committedEnd, maxHeapEnd,
        committedRegionCount, maxVirtualRegionCount, shardCount, scaleConfig.processorsPerShard,
        scaleConfig.initialSegmentCount,
        segmentSize / (1024 * 1024),
        committedUnitCount, maxVirtualUnitCount, unitsPerMSRegion_);
}

void MSRegionManager::InitializeFreshRegions(RegionIndex startIdx, RegionCount count)
{
    size_t startRegionIdx = static_cast<size_t>(startIdx);
    size_t regionCount = static_cast<size_t>(count);
    for (size_t i = 0; i < regionCount; ++i) {
        size_t regionIdx = startRegionIdx + i;
        // A grown segment can reuse a previously shrunk virtual range. Reset the
        // whole region entry, including the tracked location/owner side tables,
        // so stale CENTRAL_* state cannot survive into the next fresh take.
        array_[regionIdx].Reset();
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
uint64_t GetHeuRequestCooldownNs()
{
    return CangjieRuntime::GetGCParam().gcInterval;
}

}

void MSRegionManager::MaybeNotifySweep(const CentralRegionPool* centralPool, uint8_t cls)
{
    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }

    if (centralPool == nullptr) {
        return;
    }
    BackgroundProcessor& backgroundProcessor = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
    // Always notify background sweep once mutators start probing unswept supply.
    // The helper side decides whether more slices are still worthwhile.
    backgroundProcessor.NotifyToSweep(cls);
}

size_t MSRegionManager::GetHotFreeBytes() const
{
    return freeManager_.GetHotFreeBytes();
}

size_t MSRegionManager::GetHotFreeBytesReference() const
{
    return freeManager_.GetHotFreeBytesReference();
}

uint16_t MSRegionManager::ResolveHomeShardId(uint16_t shardHint) const
{
    MSShardRouteContext context {};
    context.shardCount = freeManager_.GetShardCount();
    context.hasBoundProcessor = ThreadLocal::IsCJProcessor();
    context.tid = static_cast<uint64_t>(GetTid());
    if (context.hasBoundProcessor) {
        context.processorId = ProcessorId();
    }
    return ResolveMSHomeShardId(shardHint, context);
}

void MSRegionManager::MaybeNotifyScavenge()
{
    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }

    Collector& collector = Heap::GetHeap().GetCollector();
    const GCStats& gcStats = collector.GetGCStats();
    size_t scavengeThreshold = gcStats.GetScavengeThreshold();
    if (scavengeThreshold == 0) {
        return;
    }

    const RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    const CentralRegionPool* pool = space.GetCentralPool();
    if (pool == nullptr) {
        return;
    }

    size_t heapRetained = space.GetHeapRetainedBytes();
    if (heapRetained <= scavengeThreshold) {
        return;
    }
    bool expectedProbe = false;
    if (!scavengePressureProbeInProgress_.compare_exchange_strong(
            expectedProbe, true, std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
    }
    // Aggregate counters keep the Region acquisition path O(1). Confirm only
    // a threshold crossing with the retained reference scan so a transient
    // cross-counter transfer cannot cause a spurious wakeup.
    if (!space.HasPendingScavengeWork()) {
        scavengePressureProbeInProgress_.store(false, std::memory_order_release);
        return;
    }

    BackgroundProcessor& bp = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
    bp.NotifyToScavenge();
    scavengePressureProbeInProgress_.store(false, std::memory_order_release);
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
    CentralRegionPool* pool = space.GetCentralPool();
    if (pool == nullptr) {
        return;
    }

    // Gate the next GC on the swept-retained footprint (partialSwept + fullSwept +
    // large region spans) rather than the transient active-region total, so the
    // trigger tracks the post-sweep live heap.
    size_t sweptBytes = space.GetSweptHeapBytes();
    size_t threshold = gcStats.GetThreshold();

    // Trigger purely on time (cooldown above) + threshold.
    if (sweptBytes < threshold) {
        return;
    }
    bool expectedProbe = false;
    if (!markPressureProbeInProgress_.compare_exchange_strong(
            expectedProbe, true, std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
    }
    // The fast aggregate may briefly lead the underlying lists while a Region
    // changes ownership. A HEU request is expensive, so confirm the crossing
    // with the original per-class reference scan.
    sweptBytes = space.GetSweptHeapBytesReference();
    if (sweptBytes < threshold) {
        markPressureProbeInProgress_.store(false, std::memory_order_release);
        return;
    }

    // Sweep executors refresh the shared pressure snapshot at slice boundaries.
    // The Region acquisition path only consumes that published mode.
    SweepPressureMode pressureMode = pool->GetSweepPressureMode();

    // A pressured old Sweep epoch gets extra parallel consumers, but no longer
    // gates the HEU request. If Mark wins the race, PreEnumRoots joins the same
    // claim path and closes all in-flight Sweep work before clearing bitmaps.
    if (pressureMode != SweepPressureMode::LOW) {
        BackgroundProcessor& backgroundProcessor =
            Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
        backgroundProcessor.NotifyToParallelSweep();
        VLOG(REPORT,
             "[MS-GC] HEU waking parallel Sweep: swept=%zuB threshold=%zuB mode=%d",
             sweptBytes, threshold, static_cast<int>(pressureMode));
    }

    if (!lastHeuRequestNs_.compare_exchange_strong(lastHeuRequestNs, nowNs,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
        markPressureProbeInProgress_.store(false, std::memory_order_release);
        return;
    }

    collector.RequestGC(GC_REASON_HEU, true);
    markPressureProbeInProgress_.store(false, std::memory_order_release);
    VLOG(REPORT,
         "[MS-GC] HEU trigger (take-regions): swept-region bytes=%zuB heapThreshold=%zuB"
         " reason=GC_REASON_HEU",
         sweptBytes,
         threshold);
}

// ============================================================================
// TakeRegions — allocate contiguous regions from the unified page-heap manager.
// ============================================================================

bool MSRegionManager::TakeRegions(RegionCount num, uint16_t homeShardId, RegionIndex& idx)
{
    RegionIndex rawIdx = 0;
    MaybeNotifyMark();

    FreeManager::TryTakeResult result = freeManager_.TryTakeRegions(num, homeShardId, rawIdx);
    if (result != FreeManager::TryTakeResult::SUCCESS &&
        !freeManager_.TryTakeOrGrowRegions(num, homeShardId, rawIdx, *this)) {
        return false;
    }

    idx = rawIdx;
    MaybeNotifyScavenge();
    return true;
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

MSRegionInfo* MSRegionManager::TakeRegion(uint16_t homeShardId)
{
    RegionIndex regionIdx = 0;
    if (!TakeRegions(1, homeShardId, regionIdx)) {
        return nullptr;
    }

    MRT_ASSERT(regionIdx < freeManager_.GetMaxVirtualRegionCount(), "region index out of bounds");

    MSRegionInfo* info = &array_[regionIdx];
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (UNLIKELY(!info->IsFreeRegion())) {
        LOG(RTLOG_FATAL, "[MS-GC] TakeRegion: array_[%u] NOT FREE! "
            "regionType=%d, allocPtr=0x%zx, regionEnd=0x%zx, allocBitmap=%p, "
            "sizeClass=%u, allocMode=%u, collectEpoch=%u, "
            "centralNext=%d, markTaskNext=%d, reserved=0x%llx, "
            "array_=%p, info=%p",
            regionIdx, (int)info->GetRegionType(), info->allocPtr, info->regionEnd,
            (void*)info->GetAllocBitmap(), (unsigned)info->sizeClass,
            static_cast<unsigned>(info->GetAllocMode()),
            static_cast<unsigned>(info->collectEpoch),
            static_cast<int32_t>(info->GetCentralNextRegionIdx()),
            static_cast<int32_t>(info->GetMarkTaskNextRegionIdx()),
            (unsigned long long)info->allocSummary,
            (void*)array_, (void*)info);
    }
#endif
    return info;
}

// ============================================================================
// TakeRegionForLarge — allocate N units for a large object
// ============================================================================

MSRegionInfo* MSRegionManager::TakeRegionForLarge(size_t unitCount, uintptr_t& regionStart,
                                                  uint16_t homeShardId)
{
    // Convert unit count to region count (round up to full regions).
    RegionCount regionCount = static_cast<RegionCount>(
        (unitCount + unitsPerMSRegion_ - 1) / unitsPerMSRegion_);

    RegionIndex regionIdx = 0;
    if (!TakeRegions(regionCount, homeShardId, regionIdx)) {
        regionStart = 0;
        return nullptr;
    }

    regionStart = RegionIdxToAddr(regionIdx);

    MRT_ASSERT(regionIdx < freeManager_.GetMaxVirtualRegionCount(), "region index out of bounds");

    return &array_[regionIdx];
}

// ============================================================================
// ReclaimRegionLarge — reclaim a large Region spanning regionBytes back to
// FreeManager hot-free supply. Physical page release is deferred to
// BackgroundScavengeLoop Step B.
// regionStart must be the actual allocation start address (saved before Reset).
// ============================================================================

void MSRegionManager::ReclaimRegionLarge(uintptr_t regionStart, size_t regionBytes)
{
    RegionIndex regionIdx = AddrToRegionIdx(regionStart);
    RegionCount regionCount = static_cast<RegionCount>(
        (regionBytes + MS_REGION_SIZE - 1) / MS_REGION_SIZE);
    DLOG(REGION, "[MS-GC] ReclaimRegionLarge->hotFree: regionStart=0x%zx, regionIdx=%u, regionCount=%u, "
                 "ownerShard=%u, bytes=%zuKB",
         regionStart, regionIdx, regionCount, freeManager_.ResolveOwnerShardId(regionIdx), regionBytes / 1024);

    // Large objects return to FreeManager first; BackgroundScavengeLoop decides
    // when those hot-free pages are actually released to the OS.
    freeManager_.ReclaimRegionsReleased(regionIdx, regionCount);
}

void MSRegionManager::ReclaimEmptyRegionToHotFree(MSRegionInfo* region)
{
    size_t regionIdx = static_cast<size_t>(region - array_);
    CHECK_DETAIL(region->IsFreeRegion(),
                 "[MS-GC] ReclaimEmptyRegionToHotFree: bad regionType=%u idx=%zu",
                 static_cast<unsigned>(region->GetRegionType()), regionIdx);

    region->Reset();
    DLOG(REGION, "[MS-GC] ReclaimEmptyRegionToHotFree: regionIdx=%zu ownerShard=%u",
         regionIdx, freeManager_.ResolveOwnerShardId(static_cast<RegionIndex>(regionIdx)));
    freeManager_.ReclaimRegionsReleased(static_cast<RegionIndex>(regionIdx), 1);
}

void MSRegionManager::ReclaimReleasedRegionsToHotFreeBatch(const std::vector<FreeManager::RegionSpan>& spans)
{
    freeManager_.ReclaimRegionsReleasedBatch(spans);
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
// GrowOneSegment — delegate heap growth to FreeManager.
// FreeManager updates the committed heap state and calls back into
// MSRegionManager to initialize new MSRegionInfo entries.
// ============================================================================

bool MSRegionManager::GrowOneSegment()
{
    AllocBufferMS* buf = ThreadLocal::GetAllocBufferMS();
    uint16_t shardId = (buf != nullptr && buf->GetCachedHomeShardId() != kMSInvalidShardId)
        ? buf->GetCachedHomeShardId()
        : static_cast<uint16_t>(static_cast<uint32_t>(GetTid()) % freeManager_.GetShardCount());
    return freeManager_.GrowOneSegment(shardId, *this);
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

bool MSRegionManager::HasPendingShrinkWork(const CentralRegionPool* centralPool) const
{
    if (centralPool == nullptr) {
        return false;
    }

    size_t segmentRegionCount = freeManager_.GetSegmentRegionCount();
    size_t committedSegmentCount = freeManager_.GetCommittedSegmentCount();
    size_t committedRegionCount = freeManager_.GetCommittedRegionCount();
    if (segmentRegionCount == 0 || committedSegmentCount == 0 || committedRegionCount == 0) {
        return false;
    }

    for (size_t segmentIdx = 0; segmentIdx < committedSegmentCount; ++segmentIdx) {
        size_t startRegionIdx = segmentIdx * segmentRegionCount;
        size_t endRegionIdx = std::min(startRegionIdx + segmentRegionCount, committedRegionCount);
        if (startRegionIdx >= endRegionIdx) {
            continue;
        }

        bool hasInUseRegion = false;
        bool hasEmptyCacheRegion = false;
        for (size_t regionIdx = startRegionIdx; regionIdx < endRegionIdx; ++regionIdx) {
            MSRegionType type = array_[regionIdx].GetRegionType();
            if (IsSegmentRegionTypeInUse(type)) {
                hasInUseRegion = true;
                break;
            }
            if (type == MSRegionType::MS_REGION_EMPTY_CACHE) {
                hasEmptyCacheRegion = true;
            }
        }
        if (hasInUseRegion) {
            continue;
        }

        if (hasEmptyCacheRegion || centralPool->HasIdleSegmentCachedRegions(startRegionIdx, endRegionIdx)) {
            return true;
        }

        FreeManager::SegmentFreeSnapshot snapshot = freeManager_.GetSegmentFreeSnapshot(segmentIdx);
        if (snapshot.validRegions != 0 && snapshot.freeRegions > snapshot.scavengedRegions) {
            return true;
        }
    }
    return false;
}

size_t MSRegionManager::ShrinkOneIdleSegment(CentralRegionPool* centralPool)
{
    if (centralPool == nullptr) {
        return 0;
    }

    size_t segmentRegionCount = freeManager_.GetSegmentRegionCount();
    size_t committedSegmentCount = freeManager_.GetCommittedSegmentCount();
    size_t committedRegionCount = freeManager_.GetCommittedRegionCount();
    if (segmentRegionCount == 0 || committedSegmentCount == 0 || committedRegionCount == 0) {
        return 0;
    }

    for (size_t segmentIdx = 0; segmentIdx < committedSegmentCount; ++segmentIdx) {
        size_t startRegionIdx = segmentIdx * segmentRegionCount;
        size_t endRegionIdx = std::min(startRegionIdx + segmentRegionCount, committedRegionCount);
        if (startRegionIdx >= endRegionIdx) {
            continue;
        }

        bool hasInUseRegion = false;
        for (size_t regionIdx = startRegionIdx; regionIdx < endRegionIdx; ++regionIdx) {
            if (IsSegmentRegionTypeInUse(array_[regionIdx].GetRegionType())) {
                hasInUseRegion = true;
                break;
            }
        }
        if (hasInUseRegion) {
            continue;
        }

        (void)centralPool->ReclaimIdleSegmentCaches(startRegionIdx, endRegionIdx);

        FreeManager::SegmentFreeSnapshot snapshot = freeManager_.GetSegmentFreeSnapshot(segmentIdx);
        if (snapshot.validRegions == 0 || snapshot.freeRegions != snapshot.validRegions) {
            continue;
        }

        size_t released = freeManager_.ReleaseCompletelyFreeSegment(segmentIdx);
        if (released != 0) {
            VLOG(REPORT,
                 "[MS-GC] shrink segment: seg=%zu regions=%zu released=%zuB",
                 segmentIdx,
                 snapshot.validRegions,
                 released);
            return released;
        }
    }
    return 0;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
