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
constexpr size_t DEFAULT_MS_SCAVENGE_WAKE_BYTES = 4 * MS_REGION_SIZE;

bool IsSegmentRegionTypeInUse(MSRegionType type)
{
    switch (type) {
        case MSRegionType::MS_REGION_BUMP:
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

size_t MSRegionManager::ComputeMaxVirtualHeapSize(size_t heapSize, size_t requestedVirtualHeapSize)
{
    size_t minVirtualHeapSize = RoundUp<size_t>(heapSize, MS_REGION_SIZE);
    size_t configuredVirtualHeapSize = RoundUp<size_t>(requestedVirtualHeapSize, MS_REGION_SIZE);
    return std::max(minVirtualHeapSize, configuredVirtualHeapSize);
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
    uint32_t shardCount = Runtime::CurrentRef() != nullptr
        ? Runtime::Current().GetConcurrencyModel().GetProcessorNum()
        : 1;

    // --- Max virtual capacity (full mmap reservation) ---
    size_t maxMetadataSize = GetMetadataSize(maxVirtualHeapSize);
    size_t maxHeapAligned = RoundUp<size_t>(maxVirtualHeapSize, MS_REGION_SIZE);
    size_t maxVirtualRegionCount = maxHeapAligned / MS_REGION_SIZE;
    size_t maxVirtualUnitCount = maxHeapAligned / unitSize_;

    // --- Initial committed heap: one 64MB segment per shard/P ---
    size_t segmentSize = RoundUp<size_t>(kMSShardSegmentBytes, MS_REGION_SIZE);
    size_t committedHeapSize = RoundUp<size_t>(ComputeMSInitialCommittedHeapBytes(shardCount), MS_REGION_SIZE);
    CHECK_DETAIL(committedHeapSize <= maxHeapAligned,
                 "[MS-GC] initial committed heap exceeds max virtual heap: committed=%zu maxVirtual=%zu shardCount=%u",
                 committedHeapSize, maxHeapAligned, shardCount);
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
        array_[i].regionType = MSRegionType::MS_REGION_FREE;
        array_[i].nextRegionIdx = MSRegionInfo::INVALID_IDX;
        array_[i].prevRegionIdx = MSRegionInfo::INVALID_IDX;
    }

    freeManager_.Initialize(static_cast<UnitCount>(maxVirtualUnitCount), unitSize_, unitsPerMSRegion_,
                            static_cast<RegionCount>(committedRegionCount),
                            static_cast<RegionCount>(maxVirtualRegionCount),
                            heapStart_, committedEnd, maxHeapEnd, segmentSize, shardCount);
    MSBitmapArena::Init(maxVirtualRegionCount, committedRegionCount);

    VLOG(REPORT, "[MS-GC] MSRegionManager initialized (virtual reserve): mmapBase=0x%zx, "
        "maxMetadata=%zuKB, heapStart=0x%zx, committedEnd=0x%zx, maxHeapEnd=0x%zx, "
        "committedRegions=%zu, maxVirtualRegions=%zu, shardCount=%u, segmentSize=%zuMB, "
        "committedUnits=%zu, maxVirtualUnits=%zu, unitsPerMSRegion=%zu",
        mmapBase, maxMetadataSize / 1024,
        heapStart_, committedEnd, maxHeapEnd,
        committedRegionCount, maxVirtualRegionCount, shardCount,
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

size_t GetReleasedBusyRetryLimit()
{
    return CangjieRuntime::GetGCParam().msReleasedBusyRetryLimit;
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

uint16_t MSRegionManager::ResolveHomeShardId(uint16_t shardHint) const
{
    MSShardRouteContext context {};
    if (Runtime::CurrentRef() != nullptr) {
        context.shardCount = Runtime::Current().GetConcurrencyModel().GetProcessorNum();
    }
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

    BackgroundProcessor& bp = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
    bp.NotifyToScavenge();
}

void MSRegionManager::MaybeNotifyShrink()
{
    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }

    const RegionSpaceMS& space = static_cast<const RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    if (space.GetCentralPool() == nullptr) {
        return;
    }

    // Keep the mutator grow path non-blocking. The full shrink-work probe scans
    // committed segments and CentralRegionPool caches, and can contend with the
    // background shrink thread on central-pool locks during large-object bursts.
    // Request the helper thread once and let it perform the heavyweight check.
    BackgroundProcessor& bp = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
    bp.NotifyToShrink();
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

    // Refresh the sweep-debt pressure mode on every probe (Step34 §18.2-18.3): below
    // threshold this latches NORMAL/BACKLOG with the real runway for assist/background
    // to read; at/above threshold the decision also tells us whether to defer HEU.
    SweepDebtDecision debt = pool->RefreshSweepDebtSnapshot(threshold, sweptBytes);
    MSHeuExecutionDecision decision = EvaluateMSHeuExecution(sweptBytes, threshold, debt);

    // Trigger purely on time (cooldown above) + threshold.
    if (decision == MSHeuExecutionDecision::BELOW_THRESHOLD) {
        return;
    }

    // backlog-aware HEU gating: if the previous cycle's unswept backlog is still
    // large (and sweep is not done), defer this HEU GC and repay debt first, rather
    // than letting StopAndWaitForAllSweep->ForceFinishAllUnswept drain it serially
    // inside the next GC's pre-mark STW (the tail-pause source). Only GC_REASON_HEU
    // is gated; OOM/FORCE/HEU_SYNC are unaffected. The backlog is fixed for the
    // cycle and is being drained by background/assist at the elevated pressure tier,
    // so deferral is bounded — once sweepDone or the backlog drops below the exit
    // waterline, the next probe lets HEU proceed.
    if (decision == MSHeuExecutionDecision::DEFER_FOR_SWEEP) {
        BackgroundProcessor& backgroundProcessor =
            Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
        backgroundProcessor.NotifyToParallelSweep(DeferredHeuOrigin::SUBMIT_SIDE);
        VLOG(REPORT,
             "[MS-GC] HEU deferred by sweep backlog: swept=%zuB threshold=%zuB mode=%d",
             sweptBytes, threshold, static_cast<int>(debt.mode));
        return;
    }

    if (!lastHeuRequestNs_.compare_exchange_strong(lastHeuRequestNs, nowNs,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
        return;
    }

    collector.RequestGC(GC_REASON_HEU, true);
    VLOG(REPORT,
         "[MS-GC] HEU trigger (take-regions): swept-region bytes=%zuB heapThreshold=%zuB"
         " reason=GC_REASON_HEU",
         sweptBytes,
         threshold);
}

// ============================================================================
// TakeRegions — allocate contiguous regions from the unified page-heap manager.
// ============================================================================

bool MSRegionManager::TakeRegions(RegionCount num, uint16_t homeShardId, RegionIndex& idx, AllocSource* source)
{
    RegionIndex rawIdx = 0;
    MaybeNotifyMark();

    while (true) {
        FreeManager::TryTakeResult result = freeManager_.TryTakeRegions(num, homeShardId, rawIdx);
        if (result == FreeManager::TryTakeResult::SUCCESS) {
            idx = rawIdx;
            if (source != nullptr) {
                *source = AllocSource::RELEASED;
            }
            MaybeNotifyScavenge();
            return true;
        }

        if (!freeManager_.GrowForRegions(num, homeShardId, *this)) {
            return false;
        }
        MaybeNotifyShrink();
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

MSRegionInfo* MSRegionManager::TakeRegion(uint8_t cls, const CentralRegionPool& centralPool,
                                          uint16_t homeShardId)
{
    (void)centralPool;
    RegionIndex regionIdx = 0;
    AllocSource source = AllocSource::BUMP;
    if (!TakeRegions(1, homeShardId, regionIdx, &source)) {
        return nullptr;
    }

    MRT_ASSERT(regionIdx < freeManager_.GetMaxVirtualRegionCount(), "region index out of bounds");

    MSRegionInfo* info = &array_[regionIdx];
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
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
    AllocSource source = AllocSource::BUMP;
    if (!TakeRegions(regionCount, homeShardId, regionIdx, &source)) {
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
    uint16_t ownerShardId = freeManager_.ResolveOwnerShardId(regionIdx);

    DLOG(REGION, "[MS-GC] ReclaimRegionLarge->hotFree: regionStart=0x%zx, regionIdx=%u, regionCount=%u, "
                 "ownerShard=%u, bytes=%zuKB",
         regionStart, regionIdx, regionCount, ownerShardId, regionBytes / 1024);

    // Large objects return to FreeManager first; BackgroundScavengeLoop decides
    // when those hot-free pages are actually released to the OS.
    freeManager_.ReclaimRegionsReleased(regionIdx, regionCount);
}

void MSRegionManager::ReclaimEmptyRegionToHotFree(MSRegionInfo* region)
{
    size_t regionIdx = static_cast<size_t>(region - array_);
    uint16_t ownerShardId = freeManager_.ResolveOwnerShardId(static_cast<RegionIndex>(regionIdx));
    CHECK_DETAIL(region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE,
                 "[MS-GC] ReclaimEmptyRegionToHotFree: bad regionType=%u idx=%zu",
                 static_cast<unsigned>(region->regionType), regionIdx);

    region->Reset();
    DLOG(REGION, "[MS-GC] ReclaimEmptyRegionToHotFree: regionIdx=%zu ownerShard=%u",
         regionIdx, ownerShardId);
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
// ReleaseGarbageRegions — no-op for MSGC.
// Small regions are kept in CentralRegionPool empty cache and reclaimed from
// there. Large regions are returned through ReclaimRegionLarge() into
// FreeManager hot-free supply, then scavenged later by BackgroundScavengeLoop.
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
            MSRegionType type = array_[regionIdx].regionType;
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
            if (IsSegmentRegionTypeInUse(array_[regionIdx].regionType)) {
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
