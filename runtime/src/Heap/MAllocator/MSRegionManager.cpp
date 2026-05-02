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
#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "AllocUtil.h"
#include "Heap/MAllocator/MSBitmapArena.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"
#include "Collector/Collector.h"
#include "Collector/CollectorResources.h"

namespace MapleRuntime {

static constexpr bool kDisableSweepOnRegionMissProbe = true;

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

size_t ReadEnvSizeBytes(const char* name, size_t fallback)
{
    const char* env = std::getenv(name);
    if (env == nullptr) {
        return fallback;
    }
    size_t valueKb = CString::ParseSizeFromEnv(env);
    return valueKb == 0 ? fallback : valueKb * KB;
}

uint64_t GetHeuRequestCooldownNs()
{
    static const uint64_t value = ReadEnvTimeNs("CJ_MS_HEU_REQUEST_COOLDOWN", 10ULL * 1000ULL * 1000ULL);
    return value;
}

size_t GetHeuReadyRegionWatermark()
{
    static const size_t value = ReadEnvCount("CJ_MS_HEU_READY_WATERMARK", 4);
    return value;
}

size_t GetHeuUnsweptRegionWatermark()
{
    static const size_t value = ReadEnvCount("CJ_MS_HEU_UNSWEPT_WATERMARK", 16);
    return value;
}

size_t GetHeuReleasedTreeBytesWatermark()
{
    static const size_t value = ReadEnvSizeBytes("CJ_MS_HEU_RELEASED_TREE_BYTES_WATERMARK",
                                                 16ULL * 1024ULL * 1024ULL);
    return value;
}

uint32_t GetHeuReleasedTreeNodeWatermark()
{
    static const uint32_t value = static_cast<uint32_t>(ReadEnvCount("CJ_MS_HEU_RELEASED_TREE_NODE_WATERMARK", 128));
    return value;
}

uint32_t GetHeuReleasedTreeMaxBlockWatermark()
{
    static const uint32_t value = static_cast<uint32_t>(ReadEnvCount("CJ_MS_HEU_RELEASED_TREE_MAX_BLOCK_WATERMARK", 8));
    return value;
}

// 方案4: Sweep-lag early trigger threshold (in number of regions).
// If the unswept backlog exceeds this count, trigger a new GC early to
// prevent RSS ballooning. Default: 64 regions = 64 * 256KB = 16MB backlog.
size_t GetHeuSweepLagTriggerRegions()
{
    static const size_t value = ReadEnvCount("CJ_MS_HEU_SWEEP_LAG_TRIGGER_REGIONS", 64);
    return value;
}

size_t GetBgSweepNotifyReadyWatermark()
{
    static const size_t value = ReadEnvCount("CJ_MS_BG_SWEEP_NOTIFY_READY_WATERMARK", 8);
    return value;
}

bool GetInlineReclaimEnabled()
{
    static const bool value = ReadEnvCount("CJ_MS_INLINE_RECLAIM", 1) != 0;
    return value;
}
}

void MSRegionManager::MaybeNotifySweep(const CentralRegionPool* centralPool, uint8_t cls)
{
    BackgroundProcessor& backgroundProcessor = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();

    // Unconditional kick when called without centralPool (GC_CONCURRENT_SWEEP path).
    if (centralPool == nullptr) {
        backgroundProcessor.NotifyToSweep();
        return;
    }

    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }

    CentralRegionPool::BucketPressure pressure = centralPool->GetBucketPressure(cls);
    if (pressure.ReadyCount() < GetBgSweepNotifyReadyWatermark() && pressure.UnsweptCount() > 0) {
        backgroundProcessor.NotifyToSweep();
    }
}

void MSRegionManager::MaybeNotifyReclaim()
{
    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }

    const CentralRegionPool* pool =
        static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator()).GetCentralPool();
    if (pool == nullptr) {
        return;
    }

    size_t emptyCount = pool->GetEmptySweptRegionCount();
    size_t highWatermark = pool->GetEmptyCacheHighWatermarkRegions();
    bool shouldReclaim = emptyCount >= highWatermark;
    if (!shouldReclaim) {
        size_t committedBytes = pool->GetTotalMSRegionBytes();
        size_t targetBytes = pool->GetTrueLiveSlackTargetBytes();
        shouldReclaim = targetBytes != 0 && committedBytes > targetBytes && emptyCount > 0;
    }

    if (shouldReclaim) {
        BackgroundProcessor& backgroundProcessor = Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor();
        backgroundProcessor.NotifyToReclaimGarbage();
    }
}

void MSRegionManager::RecordReclaimPressure()
{
    if (!pendingReclaimPressure_.exchange(true, std::memory_order_relaxed)) {
        Heap::GetHeap().GetCollectorResources().GetBackgroundProcessor().NotifyToFeedAllocBuffers();
    }
}

void MSRegionManager::DrainBackgroundReclaimPressure()
{
    if (pendingReclaimPressure_.exchange(false, std::memory_order_relaxed)) {
        MaybeNotifyReclaim();
    }
}

void MSRegionManager::MaybeNotifyMark(HeuTriggerKind triggerKind, const CentralRegionPool* centralPool,
                                        uint8_t cls, bool bumpSource)
{
    if (Heap::GetHeap().IsGcStarted()) {
        return;
    }

    Collector& collector = Heap::GetHeap().GetCollector();
    const GCStats& gcStats = collector.GetGCStats();
    size_t threshold = gcStats.GetThreshold();
    size_t allocated = Heap::GetHeap().GetAllocator().AllocatedBytes();
    size_t baseline = gcStats.baselineAllocatedBytes;
    size_t newAlloc = (allocated > baseline) ? (allocated - baseline) : 0;
    size_t effectiveAlloc = gcStats.lastTrueLiveBytes + newAlloc;

    bool thresholdTriggered = effectiveAlloc >= threshold;
    size_t releasedTreeBytes = GetReleasedTreeBytes();
    uint32_t releasedTreeNodeCount = GetReleasedTreeNodeCount();
    uint32_t releasedTreeMaxBlock = GetReleasedTreeMaxBlock();
    bool pressureTriggered = false;
    uint8_t triggeredCls = cls;
    size_t triggeredReady = 0;
    size_t triggeredUnswept = 0;
    size_t emptyCacheCount = 0;

    auto releasedPressureLow = [&]() {
                return releasedTreeBytes < GetHeuReleasedTreeBytesWatermark() &&
                             !(releasedTreeNodeCount >= GetHeuReleasedTreeNodeWatermark() &&
                                 releasedTreeMaxBlock >= GetHeuReleasedTreeMaxBlockWatermark());
    };

    switch (triggerKind) {
        case HeuTriggerKind::SMALL_ALLOC: {
            CHECK_DETAIL(centralPool != nullptr, "SMALL_ALLOC HEU trigger requires centralPool");
            CentralRegionPool::BucketPressure pressure = centralPool->GetBucketPressure(cls);
            triggeredReady = pressure.ReadyCount();
            triggeredUnswept = pressure.UnsweptCount();
            emptyCacheCount = centralPool->GetEmptySweptRegionCount();
            if (!thresholdTriggered && bumpSource) {
                bool readyLow = triggeredReady < GetHeuReadyRegionWatermark();
                bool unsweptLow = triggeredUnswept < GetHeuUnsweptRegionWatermark();
                bool emptyLow = centralPool->IsEmptyCacheLow();
                pressureTriggered = readyLow && unsweptLow && emptyLow && releasedPressureLow();
            }
            break;
        }
        case HeuTriggerKind::LARGE_ALLOC:
            break;
        case HeuTriggerKind::BG_SWEEP_EXHAUSTED: {
            CHECK_DETAIL(centralPool != nullptr, "BG_SWEEP_EXHAUSTED HEU trigger requires centralPool");
            emptyCacheCount = centralPool->GetEmptySweptRegionCount();
            if (!thresholdTriggered) {
                bool emptyLow = centralPool->IsEmptyCacheLow();
                bool releasedLow = releasedPressureLow();
                for (uint8_t candidateCls = 0; candidateCls < kNumSizeClasses; candidateCls++) {
                    CentralRegionPool::BucketPressure pressure = centralPool->GetBucketPressure(candidateCls);
                    bool readyLow = pressure.ReadyCount() < GetHeuReadyRegionWatermark();
                    bool unsweptLow = pressure.UnsweptCount() < GetHeuUnsweptRegionWatermark();
                    if (readyLow && unsweptLow && emptyLow && releasedLow) {
                        pressureTriggered = true;
                        triggeredCls = candidateCls;
                        triggeredReady = pressure.ReadyCount();
                        triggeredUnswept = pressure.UnsweptCount();
                        break;
                    }
                }
            }
            if (thresholdTriggered) {
                triggeredUnswept = centralPool->GetTotalUnsweptCount();
            }
            break;
        }
    }

    // 方案4: Sweep-lag early trigger.
    // If the unswept backlog is very large (background sweep can't keep up with
    // allocation rate), trigger a new GC earlier than the normal threshold would.
    // This avoids RSS ballooning caused by the heap growing faster than sweep.
    if (!thresholdTriggered && !pressureTriggered && centralPool != nullptr) {
        size_t totalUnswept = centralPool->GetTotalUnsweptCount();
        if (totalUnswept > GetHeuSweepLagTriggerRegions()) {
            pressureTriggered = true;
            triggeredUnswept = totalUnswept;
        }
    }

    if (!thresholdTriggered && !pressureTriggered) {
        return;
    }
    uint64_t nowNs = TimeUtil::NanoSeconds();
    uint64_t lastHeuRequestNs = lastHeuRequestNs_.load(std::memory_order_relaxed);
    if (lastHeuRequestNs != 0 && nowNs - lastHeuRequestNs < GetHeuRequestCooldownNs()) {
        return;
    }
    if (!lastHeuRequestNs_.compare_exchange_strong(lastHeuRequestNs, nowNs,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
        return;
    }

    collector.RequestGC(GC_REASON_HEU, true);
    switch (triggerKind) {
        case HeuTriggerKind::SMALL_ALLOC:
            VLOG(REPORT,
                 "[MS-GC] HEU trigger: cls=%u source=%u effectiveAlloc=%zu threshold=%zu ready=%zu unswept=%zu "
                 "emptyCache=%zu releasedTree=%zuB nodes=%u maxBlock=%u reason=%s",
                 static_cast<unsigned>(cls),
                 static_cast<unsigned>(bumpSource ? AllocSource::BUMP : AllocSource::RELEASED),
                 effectiveAlloc, threshold,
                 triggeredReady, triggeredUnswept,
                 emptyCacheCount,
                 releasedTreeBytes, releasedTreeNodeCount, releasedTreeMaxBlock,
                 thresholdTriggered ? "threshold" : (triggeredUnswept > GetHeuSweepLagTriggerRegions() ? "sweep-lag" : "pressure"));
            break;
        case HeuTriggerKind::LARGE_ALLOC:
            VLOG(REPORT,
                 "[MS-GC] HEU trigger (large): source=%u effectiveAlloc=%zu threshold=%zu releasedTree=%zuB nodes=%u maxBlock=%u reason=%s",
                 static_cast<unsigned>(bumpSource ? AllocSource::BUMP : AllocSource::RELEASED),
                 effectiveAlloc, threshold,
                 releasedTreeBytes, releasedTreeNodeCount, releasedTreeMaxBlock,
                 thresholdTriggered ? "threshold" : (triggeredUnswept > GetHeuSweepLagTriggerRegions() ? "sweep-lag" : "pressure"));
            break;
        case HeuTriggerKind::BG_SWEEP_EXHAUSTED:
            VLOG(REPORT,
                 "[MS-GC] HEU trigger (bg-sweep exhausted): effectiveAlloc=%zu threshold=%zu ready=%zu unswept=%zu "
                 "emptyCache=%zu releasedTree=%zuB nodes=%u maxBlock=%u reason=%s cls=%u",
                 effectiveAlloc, threshold,
                 pressureTriggered ? triggeredReady : 0,
                 triggeredUnswept,
                 emptyCacheCount,
                 releasedTreeBytes, releasedTreeNodeCount, releasedTreeMaxBlock,
                 thresholdTriggered ? "threshold" : "pressure",
                 static_cast<unsigned>(triggeredCls));
            break;
    }
}

// ============================================================================
// TakeRegions — allocate contiguous regions from releasedTree or inactiveZone bump.
// FreeManager exposes the primitive heap-state operations, while the policy and
// fallback sequencing remain in MSRegionManager.
// ============================================================================

bool MSRegionManager::TakeRegions(RegionCount num, RegionIndex& idx, AllocSource* source)
{
    RegionIndex rawIdx = 0;
    bool tryReleasedTree = true;

    while (tryReleasedTree) {
        FreeManager::TryTakeResult result = freeManager_.TryTakeRegions(num, rawIdx);
        if (result == FreeManager::TryTakeResult::SUCCESS) {
            idx = rawIdx;
            if (source != nullptr) {
                *source = AllocSource::RELEASED;
            }
            allocFromReleasedCount_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (result == FreeManager::TryTakeResult::EMPTY) {
            tryReleasedTree = false;
            continue;
        }
        ScopedEnterSaferegion enterSaferegion(true);
    }

    while (true) {
        if (freeManager_.TryIncreaseInactiveZone(num, rawIdx)) {
            idx = rawIdx;
            if (source != nullptr) {
                *source = AllocSource::BUMP;
            }
            allocFromBumpCount_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        if (!freeManager_.GrowOneSegment(*this)) {
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

// ============================================================================
// TakeRegion — allocate one MS_REGION_SIZE region
// ============================================================================

MSRegionInfo* MSRegionManager::TakeRegion(uint8_t cls, const CentralRegionPool& centralPool)
{
    RegionIndex regionIdx = 0;
    AllocSource source = AllocSource::BUMP;
    if (!TakeRegions(1, regionIdx, &source)) {
        return nullptr;
    }

    if (!kDisableSweepOnRegionMissProbe) {
        MaybeNotifySweep(&centralPool, cls);
    }
    RecordReclaimPressure();
    MaybeNotifyMark(HeuTriggerKind::SMALL_ALLOC, &centralPool, cls, source == AllocSource::BUMP);

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
    if (!TakeRegions(regionCount, regionIdx, &source)) {
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

    RecordReclaimPressure();
    MaybeNotifyMark(HeuTriggerKind::LARGE_ALLOC, nullptr, 0, source == AllocSource::BUMP);

    regionStart = RegionIdxToAddr(regionIdx);

    MRT_ASSERT(regionIdx < freeManager_.GetMaxVirtualRegionCount(), "region index out of bounds");

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

    freeManager_.ReclaimRegionsReleased(static_cast<RegionIndex>(regionIdx), 1);
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

void MSRegionManager::ReclaimEmptyRegionToReleased(MSRegionInfo* region)
{
    size_t regionIdx = static_cast<size_t>(region - array_);
    CHECK_DETAIL(region->regionType == MSRegionType::MS_REGION_EMPTY_CACHE,
                 "[MS-GC] ReclaimEmptyRegionToReleased: bad regionType=%u idx=%zu",
                 static_cast<unsigned>(region->regionType), regionIdx);

    region->Reset();
    freeManager_.ReclaimRegionsReleased(static_cast<RegionIndex>(regionIdx), 1);
}

// ============================================================================
// InlineReclaimForBalance — synchronously release one empty-cache region
// to offset a BUMP allocation, keeping committed bytes stable.
// ============================================================================

void MSRegionManager::InlineReclaimForBalance(const CentralRegionPool& pool)
{
    size_t emptyCount = pool.GetEmptySweptRegionCount();
    size_t lowWatermark = pool.GetEmptyCacheLowWatermarkRegions();
    if (emptyCount <= lowWatermark) {
        return;
    }

    CentralRegionPool& mutablePool = const_cast<CentralRegionPool&>(pool);
    MSRegionInfo* region = mutablePool.TakeEmptySweptRegionForReclaim();
    if (region == nullptr) {
        return;
    }

    ReclaimEmptyRegionToReleased(region);
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
