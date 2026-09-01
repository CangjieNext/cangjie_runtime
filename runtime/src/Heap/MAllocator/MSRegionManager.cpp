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
#include "AllocUtil.h"
#include "Heap/Allocator/MemMap.h"
#include "Heap/MAllocator/MSBitmapArena.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Heap/MCollector/MSHeapAccounting.h"
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

    // Memory layout: [MSRegionInfo[maxVirtualRegionCount] | Heap[maxHeapAligned]]
    //                ^mmapBase                            ^heapStart             ^maxHeapEnd
    array_ = reinterpret_cast<MSRegionInfo*>(mmapBase);
    heapStart_ = mmapBase + maxMetadataSize;
    uintptr_t committedEnd = heapStart_ + committedHeapSize;
    uintptr_t maxHeapEnd = heapStart_ + maxHeapAligned;

#ifdef _WIN64
    // MapMemory reserves address space on Windows. Commit the metadata and the
    // initial heap segment before either range is touched.
    CHECK_DETAIL(MemMap::CommitMemory(array_, maxMetadataSize),
                 "[MS-GC] failed to commit region metadata: addr=%p size=%zu", array_, maxMetadataSize);
    CHECK_DETAIL(MemMap::CommitMemory(reinterpret_cast<void*>(heapStart_), committedHeapSize),
                 "[MS-GC] failed to commit initial heap: addr=%p size=%zu", reinterpret_cast<void*>(heapStart_),
                 committedHeapSize);
#endif

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

    VLOG(REPORT,
         "[MS-GC] heap initialized: committedRegions=%zu committed=%.2fMB maxVirtualRegions=%zu "
         "segment=%.2fMB shards=%u processorsPerShard=%u heap=[0x%zx,0x%zx)",
         committedRegionCount, MSBytesToMB(committedHeapSize), maxVirtualRegionCount,
         MSBytesToMB(segmentSize), shardCount, scaleConfig.processorsPerShard,
         heapStart_, maxHeapEnd);
}

void MSRegionManager::InitializeFreshRegions(RegionIndex startIdx, RegionCount count)
{
    size_t startRegionIdx = static_cast<size_t>(startIdx);
    size_t regionCount = static_cast<size_t>(count);
    for (size_t i = 0; i < regionCount; ++i) {
        size_t regionIdx = startRegionIdx + i;
        // A grown segment can reuse a released virtual range. Reset the whole
        // region entry, including its tracked location/owner side tables, so
        // stale CENTRAL_* state cannot survive into the next fresh take.
        array_[regionIdx].Reset();
    }
}

void MSRegionManager::OnCommittedHeapExtended(uintptr_t newCommittedEnd)
{
    Heap::OnHeapExtended(newCommittedEnd);
}

size_t MSRegionManager::GetHotFreeBytes() const
{
    return freeManager_.GetHotFreeBytes();
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
    const RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    const CentralRegionPool* pool = space.GetCentralPool();
    if (pool == nullptr) {
        return;
    }

    MSHeapRetentionSnapshot retention = CaptureMSHeapRetentionSnapshot(space);
    MSHeapPolicySnapshot policy = LoadMSHeapPolicySnapshot(gcStats);
    if (policy.scavengeLowBytes == 0 || policy.scavengeHighBytes == 0) {
        return;
    }
    if (retention.RetainedBytes() <= policy.scavengeHighBytes) {
        return;
    }
    bool expectedProbe = false;
    if (!scavengePressureProbeInProgress_.compare_exchange_strong(
            expectedProbe, true, std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
    }
    // Aggregate counters keep the Region acquisition path O(1). Re-sample the
    // Scavenge release target after winning the probe so a concurrent cache
    // transfer cannot cause a stale wakeup.
    retention = CaptureMSHeapRetentionSnapshot(space);
    policy = LoadMSHeapPolicySnapshot(gcStats);
    size_t releaseTargetBytes = ComputeMSScavengeReleaseTarget(retention, policy);
    if (releaseTargetBytes == 0) {
        scavengePressureProbeInProgress_.store(false, std::memory_order_release);
        return;
    }

    BackgroundProcessor& bp = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
    if (bp.NotifyToScavenge()) {
        VLOG(REPORT,
             "[MS-GC] Scavenge notify accepted: source=allocation-pressure "
             "retained=%.2fMB reclaimable=%.2fMB low=%.2fMB high=%.2fMB target=%.2fMB",
             MSBytesToMB(retention.RetainedBytes()),
             MSBytesToMB(retention.ReclaimableBytes()),
             MSBytesToMB(policy.scavengeLowBytes),
             MSBytesToMB(policy.scavengeHighBytes), MSBytesToMB(releaseTargetBytes));
    }
    scavengePressureProbeInProgress_.store(false, std::memory_order_release);
}

void MSRegionManager::MaybeNotifyMark()
{
    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }

    Collector& collector = Heap::GetHeap().GetCollector();
    const GCStats& gcStats = collector.GetGCStats();
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    CentralRegionPool* pool = space.GetCentralPool();
    if (pool == nullptr) {
        return;
    }

    // The normal HEU path counts only central small Regions returned after
    // mutator use plus live LARGE spans. Sweep promotion alone cannot advance
    // this trigger. A configured soft limit still uses retained physical bytes.
    MSHeapRetentionSnapshot retention = CaptureMSHeapRetentionSnapshot(space);
    MSMarkTriggerSnapshot markTrigger = CaptureMSMarkTriggerSnapshot(*pool);
    MSHeapPolicySnapshot policy = LoadMSHeapPolicySnapshot(gcStats);
    MSMarkTriggerDecision markDecision = EvaluateMSMarkTrigger(retention, markTrigger, policy);

    if (markDecision.trigger == MSMarkTrigger::NONE) {
        return;
    }

    if (markDecision.trigger == MSMarkTrigger::OOM) {
        collector.RequestGC(GC_REASON_OOM, false);
        return;
    }

    // Sweep executors refresh the shared pressure snapshot at slice boundaries.
    // The Region acquisition path only consumes that published mode.
    SweepPressureMode pressureMode = pool->GetSweepPressureMode();

    // A pressured old Sweep epoch gets extra parallel consumers, but no longer
    // gates the HEU request. If Mark wins the race, PreEnumRoots joins the same
    // claim path and closes all in-flight Sweep work before clearing bitmaps.
    if (pressureMode != SweepPressureMode::LOW) {
        MSSweepProgressSnapshot sweep = CaptureMSSweepProgressSnapshot(*pool);
        BackgroundProcessor& backgroundProcessor =
            Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
        if (backgroundProcessor.NotifyToParallelSweep()) {
            VLOG(REPORT,
                 "[MS-GC] Sweep notify accepted: source=mark-pressure epoch=%u "
                 "pressure=%u unsweptRegions=%zu unswept=%.2fMB swept=%.2fMB threshold=%.2fMB",
                 sweep.epochId, static_cast<unsigned>(pressureMode),
                 sweep.unsweptRegions, MSBytesToMB(sweep.unsweptBytes),
                 MSBytesToMB(sweep.sweptBytes), MSBytesToMB(policy.markThresholdBytes));
        }
    }

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    if (resources.TryRequestAsyncGC(GC_REASON_HEU) ==
        AsyncGCEnqueueResult::ACCEPTED) {
        VLOG(REPORT,
             "[MS-GC] Mark notify accepted: reason=heuristic trigger=%u epoch=%u "
             "retained=%.2fMB triggerUsed=%.2fMB mutatorReturnedSweptRegions=%zu "
             "largeLiveSpans=%zu threshold=%.2fMB softLimit=%.2fMB",
             static_cast<unsigned>(markDecision.trigger), pool->GetSweepCycleId(),
             MSBytesToMB(markDecision.retainedBytes),
             MSBytesToMB(markDecision.triggerBytes),
             markDecision.mutatorReturnedSweptRegions,
             markDecision.largeLiveSpanRegions,
             MSBytesToMB(markDecision.thresholdBytes),
             MSBytesToMB(markDecision.softLimitBytes));
    }
}

// ============================================================================
// TakeRegions — allocate contiguous regions from the unified page-heap manager.
// ============================================================================

bool MSRegionManager::TakeRegions(RegionCount num, uint16_t homeShardId, RegionIndex& idx)
{
    RegionIndex rawIdx = 0;
    MaybeNotifyMark();

    FreeManager::TryTakeResult result = freeManager_.TryTakeRegions(num, homeShardId, rawIdx);
    if (result == FreeManager::TryTakeResult::COMMIT_FAILED) {
        return false;
    }
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

void MSRegionManager::ReclaimReleasedRegionsToHotFreeBatch(const std::vector<FreeManager::RegionSpan>& spans)
{
    freeManager_.ReclaimRegionsReleasedBatch(spans);
}

void MSRegionManager::ForEachObjUnsafe(const std::function<void(BaseObject*)>& visitor) const
{
    for (size_t regionIdx = 0; regionIdx < GetMaxRegionCount(); ++regionIdx) {
        MSRegionInfo* region = &array_[regionIdx];
        switch (region->GetRegionKind()) {
            case MSRegionKind::LARGE:
                if (region->LargeObjectSize() != 0 &&
                    region->LargeObjectStart() < region->regionEnd) {
                    visitor(reinterpret_cast<BaseObject*>(region->LargeObjectStart()));
                }
                continue;
            case MSRegionKind::SPAN:
            case MSRegionKind::FREE:
                continue;
            case MSRegionKind::SMALL:
                break;
        }

        size_t totalSlotCount = region->GetTotalSlotCount();
        size_t slotSize = region->GetSlotSize();
        uintptr_t regionStart = region->RegionStart();
        if (totalSlotCount == 0 || slotSize == 0) {
            continue;
        }

        if (region->GetAllocMode() == MSAllocMode::BUMP) {
            uintptr_t allocatedEnd = region->allocPtr;
            if (allocatedEnd < regionStart || allocatedEnd > region->regionEnd) {
                continue;
            }
            for (uintptr_t objectAddr = regionStart; objectAddr < allocatedEnd; objectAddr += slotSize) {
                visitor(reinterpret_cast<BaseObject*>(objectAddr));
            }
            continue;
        }

        uint64_t* allocBitmap = region->GetAllocBitmap();
        size_t wordCount = region->GetBitmapWordCount();
        if (allocBitmap == nullptr || wordCount == 0) {
            continue;
        }
        for (size_t wordIdx = 0; wordIdx < wordCount; ++wordIdx) {
            uint64_t validMask = wordIdx + 1 == wordCount
                ? region->GetValidTailMask() : ~UINT64_C(0);
            uint64_t freeBits = allocBitmap[wordIdx] & validMask;
            uint64_t allocatedBits = ~freeBits & validMask;
            while (allocatedBits != 0) {
                size_t bitIdx = static_cast<size_t>(__builtin_ctzll(allocatedBits));
                size_t slotIdx = wordIdx * 64 + bitIdx;
                visitor(reinterpret_cast<BaseObject*>(regionStart + slotIdx * slotSize));
                allocatedBits &= allocatedBits - 1;
            }
        }
    }
}

void MSRegionManager::ForEachObjSafe(const std::function<void(BaseObject*)>& visitor) const
{
    ScopedEnterSaferegion enterSaferegion(false);
    ScopedStopTheWorld stw("visit all objects");
    ForEachObjUnsafe(visitor);
}

size_t MSRegionManager::ScavengeFreeRegionsUpTo(size_t maxBytes)
{
    return freeManager_.ScavengeAgedRegionsUpTo(maxBytes);
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
                 "[MS-GC] segment release: condition=idle+all-free+hot-free seg=%zu "
                 "regions=%zu released=%.2fMB committedRegions=%zu capacity=%.2fMB",
                 segmentIdx,
                 snapshot.validRegions,
                 MSBytesToMB(released), freeManager_.GetCommittedRegionCount(),
                 MSBytesToMB(freeManager_.GetCurrentCapacityBytes()));
            return released;
        }
    }
    return 0;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
