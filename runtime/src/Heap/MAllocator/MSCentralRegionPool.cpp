// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSCentralRegionPool.h"

#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSRegionManager.h"
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Common/BaseObject.h"
#include "Base/CString.h"
#include "Base/EnvParam.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/SysCall.h"
#include "Base/TimeUtils.h"
#include "CangjieRuntime.h"
#include "Collector/CollectorResources.h"
#include "Heap.h"
#include "Heap/MCollector/BackgroundProcessor.h"
#include "Heap/MCollector/MSMarkWorkerPolicy.h"
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace MapleRuntime {

namespace {
constexpr size_t kSweepDeadlineCheckInterval = 4;
constexpr uint64_t SWEEP_QUIESCENCE_SPIN_US = 20;

inline void SweepQuiescenceSpinPause()
{
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause");
#endif
}

void ClearSmallRegionPayload(uintptr_t regionStart)
{
    memset(reinterpret_cast<void*>(regionStart), 0, MS_REGION_SIZE);
}

}

// Central-only cycle id. It invalidates delayed pacing/helper state across
// cycles, but never classifies a Region or grants Sweep ownership.
std::atomic<uint32_t> CentralRegionPool::sweepCycleId_{1};

CentralRegionPool::CentralRegionPool(MSRegionManager* msRegionManager)
    : msRegionManager_(msRegionManager)
{
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            buckets_[cls][scan].BindAggregateCounters(&centralSmallActiveRegions_,
                                                      &centralSmallSweptRegions_,
                                                      &centralSmallUnsweptRegions_);
        }
    }
}

bool CentralRegionPool::TryEnterSweep()
{
    uint64_t oldState = sweepAdmission_.load(std::memory_order_acquire);
    for (;;) {
        if ((oldState & kSweepAdmissionClosed) != 0) {
            return false;
        }
        CHECK((oldState & kSweepAdmissionActiveMask) != kSweepAdmissionActiveMask);
        uint64_t newState = oldState + 1;
        if (sweepAdmission_.compare_exchange_weak(oldState, newState,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
            return true;
        }
    }
}

void CentralRegionPool::LeaveSweep()
{
    uint64_t oldState = sweepAdmission_.load(std::memory_order_acquire);
    for (;;) {
        CHECK((oldState & kSweepAdmissionActiveMask) != 0);
        uint64_t newState = oldState - 1;
        if (sweepAdmission_.compare_exchange_weak(oldState, newState,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
            if ((newState & kSweepAdmissionClosed) != 0 &&
                (newState & kSweepAdmissionActiveMask) == 0) {
                std::lock_guard<std::mutex> lock(sweepQuiescenceMtx_);
                sweepQuiescenceCV_.notify_one();
            }
            return;
        }
    }
}

void CentralRegionPool::CloseSweepAdmissionAndWait()
{
    // This RMW is the single linearization point for closing admission. A
    // worker either increments the active count before this operation and is
    // waited for, or observes the closed bit and cannot enter SweepRegion.
    sweepAdmission_.fetch_or(kSweepAdmissionClosed, std::memory_order_acq_rel);

    auto sweepQuiescent = [this]() {
        return (sweepAdmission_.load(std::memory_order_acquire) &
                kSweepAdmissionActiveMask) == 0;
    };
    if (sweepQuiescent()) {
        return;
    }

    uint64_t spinDeadlineNs = TimeUtil::NanoSeconds() + SWEEP_QUIESCENCE_SPIN_US * 1000;
    do {
        SweepQuiescenceSpinPause();
        if (sweepQuiescent()) {
            return;
        }
    } while (TimeUtil::NanoSeconds() < spinDeadlineNs);

    // Recheck while holding the same mutex used by the zero-count notifier.
    // This closes the predicate-to-wait window without changing the packed
    // admission word's role as the protocol's linearization point.
    std::unique_lock<std::mutex> quiescenceLock(sweepQuiescenceMtx_);
    sweepQuiescenceCV_.wait(quiescenceLock, sweepQuiescent);
}

void CentralRegionPool::OpenSweepAdmission()
{
    uint64_t state = sweepAdmission_.load(std::memory_order_acquire);
    CHECK((state & kSweepAdmissionActiveMask) == 0);
    CHECK((state & kSweepAdmissionClosed) != 0);
    sweepAdmission_.store(0, std::memory_order_release);
}

void CentralRegionPool::PublishSweepDoneAfterQuiescence()
{
    uint64_t state = sweepAdmission_.load(std::memory_order_acquire);
    CHECK((state & kSweepAdmissionClosed) != 0);
    CHECK((state & kSweepAdmissionActiveMask) == 0);
    CHECK(!forceFinishSweepRunning_.load(std::memory_order_acquire));
    CHECK(!bgSweepRunning_.load(std::memory_order_acquire));
    CHECK(GetTotalUnsweptCount() == 0);
    CHECK(GetTotalUnsweptCountReference() == 0);

    // Publish completion first. A concurrent ForceHighSweepPressure() either
    // observes sweepDone_ and returns, or rechecks it after setting the latch
    // and clears its own stale HIGH. Clearing the latch before this store would
    // allow the notifier to leave forced HIGH behind on an already-done epoch.
    sweepDone_.store(true, std::memory_order_release);
    forceHighSweepPressure_.store(false, std::memory_order_release);
    sweepPressureMode_.store(static_cast<int>(SweepPressureMode::LOW), std::memory_order_release);
}

std::array<uint8_t, kNumSizeClasses> CentralRegionPool::BuildBackgroundSweepClassOrder(uint8_t preferredCls) const
{
    std::array<MSSweepClassPressure, kNumSizeClasses> pressure {};
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        BucketPressure bucketPressure = GetBucketPressure(cls);
        pressure[cls] = {bucketPressure.ReadyCount(), bucketPressure.UnsweptCount()};
    }
    return BuildMSSweepPressureClassOrder(pressure, preferredCls);
}

// ============================================================================
// TakePartialSweptRegion — fastest central-pool acquisition path.
// ============================================================================
MSRegionInfo* CentralRegionPool::TakePartialSweptRegion(uint8_t cls, MSScanClass scanClass)
{
    SizeClassBucket& bucket = buckets_[cls][ToMSScanClassIndex(scanClass)];
    MSRegionInfo* region = bucket.TakePartialSwept();
    if (region == nullptr) {
        return nullptr;
    }

    size_t ridx = MSRegionInfoArray::GetRegionIdx(region);
    uintptr_t expectedEnd = MSRegionInfoArray::GetRegionAddress(ridx) + MS_REGION_SIZE;
    if (UNLIKELY(region->regionEnd != expectedEnd)) {
        LOG(RTLOG_FATAL, "[MS-GC] TakePartialSweptRegion: partialSwept region has wrong regionEnd! "
            "regionEnd=0x%zx, expected=0x%zx, allocPtr=0x%zx, cls=%u, "
            "sizeClass=%u, allocBitmap=%p, this=%p (idx=%zu)",
            region->regionEnd, expectedEnd, region->allocPtr,
            (unsigned)cls, (unsigned)region->sizeClass,
            (void*)region->GetAllocBitmap(), (void*)region, ridx);
    }
    region->SetAllocMode(MSAllocMode::BITMAP);
    return region;
}

// ============================================================================
// GetFreshRegion — allocate a brand-new Region from MSRegionManager
// ============================================================================
MSRegionInfo* CentralRegionPool::GetFreshRegion(
    uint8_t cls, MSScanClass scanClass, uint16_t shardHint)
{
    // MSRegionManager::TakeRegion allocates one MS_REGION_SIZE region
    // and returns the corresponding MSRegionInfo* directly (one-level).
    MSRegionInfo* msInfo = msRegionManager_->TakeRegion(shardHint);
    if (msInfo == nullptr) {
        return nullptr;
    }

    // Compute region start address from MSRegionInfo index
    size_t regionIdx = MSRegionInfoArray::GetRegionIdx(msInfo);
    uintptr_t regionStart = MSRegionInfoArray::GetRegionAddress(regionIdx);
    ClearSmallRegionPayload(regionStart);

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (UNLIKELY(!msInfo->IsFreeRegion())) {
        LOG(RTLOG_FATAL, "[MS-GC] GetFreshRegion: TakeRegion returned non-FREE region! "
            "regionType=%d, allocPtr=0x%zx, regionEnd=0x%zx, allocBitmap=%p, "
            "sizeClass=%u, allocMode=%u, collectEpoch=%u, "
            "centralNext=%d, markTaskNext=%d, reserved=0x%llx, "
            "cls=%u, regionIdx=%zu, regionStart=0x%zx, this=%p",
            (int)msInfo->GetRegionType(), msInfo->allocPtr, msInfo->regionEnd,
            (void*)msInfo->GetAllocBitmap(), (unsigned)msInfo->sizeClass,
            static_cast<unsigned>(msInfo->GetAllocMode()),
            static_cast<unsigned>(msInfo->collectEpoch),
            static_cast<int32_t>(msInfo->GetCentralNextRegionIdx()),
            static_cast<int32_t>(msInfo->GetMarkTaskNextRegionIdx()),
            (unsigned long long)msInfo->allocSummary,
            (unsigned)cls, regionIdx, regionStart, (void*)msInfo);
    }
#endif

    // Initialize for the requested size class (sets bitmaps, slot counts, state)
    msInfo->InitForSizeClass(cls, scanClass);

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (UNLIKELY(msInfo->GetAllocBitmap() == nullptr)) {
        LOG(RTLOG_FATAL, "[MS-GC] GetFreshRegion: InitForSizeClass failed to alloc bitmap, "
            "cls=%u, regionIdx=%zu, regionStart=0x%zx, totalSlots=%u, this=%p",
            (unsigned)cls, regionIdx, regionStart, (unsigned)msInfo->GetTotalSlotCount(),
            (void*)msInfo);
    }
#endif

    return msInfo;
}

uint16_t CentralRegionPool::ResolveHomeShardId(uint16_t shardHint) const
{
    return msRegionManager_ != nullptr ? msRegionManager_->ResolveHomeShardId(shardHint) : 0;
}

// ============================================================================
// Return / Release — Region return from thread-local cache
// ============================================================================

void CentralRegionPool::ReturnPartialRegion(MSRegionInfo* region, uint8_t cls)
{
    CHECK_DETAIL(region != nullptr, "ReturnPartialRegion with null region");
    uint8_t bucketCls = region->sizeClass;
    CHECK_DETAIL(bucketCls < kNumSizeClasses,
                 "[MS-GC] ReturnPartialRegion: invalid sizeClass=%u",
                 static_cast<unsigned>(bucketCls));
    if (UNLIKELY(cls != bucketCls)) {
        LOG(RTLOG_ERROR,
            "[MS-GC] ReturnPartialRegion: caller cls=%u mismatches region sizeClass=%u, region=%p",
            static_cast<unsigned>(cls),
            static_cast<unsigned>(bucketCls),
            static_cast<void*>(region));
    }
    SizeClassBucket& bucket =
        buckets_[bucketCls][ToMSScanClassIndex(region->GetScanClass())];
    region->SetAllocMode(MSAllocMode::BITMAP);
    region->collectEpoch = MSRegionInfo::NO_COLLECT_EPOCH;
    bucket.PushPartialSwept(region);
}

void CentralRegionPool::ReturnFullRegion(MSRegionInfo* region, uint8_t cls)
{
    CHECK_DETAIL(region != nullptr, "ReturnFullRegion with null region");
    uint8_t bucketCls = region->sizeClass;
    CHECK_DETAIL(bucketCls < kNumSizeClasses,
                 "[MS-GC] ReturnFullRegion: invalid sizeClass=%u",
                 static_cast<unsigned>(bucketCls));
    if (UNLIKELY(cls != bucketCls)) {
        LOG(RTLOG_ERROR,
            "[MS-GC] ReturnFullRegion: caller cls=%u mismatches region sizeClass=%u, region=%p",
            static_cast<unsigned>(cls),
            static_cast<unsigned>(bucketCls),
            static_cast<void*>(region));
    }
    SizeClassBucket& bucket =
        buckets_[bucketCls][ToMSScanClassIndex(region->GetScanClass())];
    region->SetAllocMode(MSAllocMode::BITMAP);
    region->collectEpoch = MSRegionInfo::NO_COLLECT_EPOCH;
    bucket.PushFullSwept(region);
}

void CentralRegionPool::ReturnEmptyRegion(MSRegionInfo* region)
{
    PushEmptySweptRegion(region);
}

void CentralRegionPool::PushEmptySweptRegion(MSRegionInfo* region)
{
    CHECK_DETAIL(region != nullptr, "PushEmptySweptRegion with null region");
    region->ResetToEmptyCache();
    {
        std::lock_guard<std::mutex> lg(emptySweptLock_);
        emptySweptCache_.Prepend(region);
    }
    IncrementEmptySweptCount();
}

MSRegionInfo* CentralRegionPool::TakeGlobalEmptyRegion()
{
    MSRegionInfo* region = nullptr;
    {
        std::lock_guard<std::mutex> lg(emptySweptLock_);
        region = emptySweptCache_.TakeHead();
    }
    if (region != nullptr) {
        DecrementEmptySweptCount();
    }
    return region;
}

size_t CentralRegionPool::DrainEmptySweptGlobal(size_t maxRegions, std::vector<MSRegionInfo*>& drained)
{
    if (maxRegions == 0) {
        return 0;
    }

    constexpr size_t kEmptyDrainBatchSize = 64;
    size_t startSize = drained.size();
    std::vector<MSRegionInfo*> batch;
    batch.reserve(std::min(maxRegions, kEmptyDrainBatchSize));

    while (drained.size() - startSize < maxRegions) {
        batch.clear();
        {
            std::lock_guard<std::mutex> lg(emptySweptLock_);
            size_t batchLimit = std::min(maxRegions - (drained.size() - startSize), kEmptyDrainBatchSize);
            for (size_t i = 0; i < batchLimit; ++i) {
                MSRegionInfo* region = emptySweptCache_.TakeHead();
                if (region == nullptr) {
                    break;
                }
                batch.push_back(region);
            }
        }
        if (batch.empty()) {
            break;
        }
        DecrementEmptySweptCount(batch.size());
        drained.insert(drained.end(), batch.begin(), batch.end());
    }

    return drained.size() - startSize;
}
size_t CentralRegionPool::ReclaimEmptyRegion(size_t maxRegions)
{
    size_t emptyCount = GetEmptySweptRegionCount();
    if (emptyCount == 0 || maxRegions == 0) {
        return 0;
    }

    std::vector<MSRegionInfo*> reclaimed;
    size_t reserveCount = std::min(emptyCount, maxRegions);
    if (reserveCount == std::numeric_limits<size_t>::max()) {
        reserveCount = 0;
    }
    reclaimed.reserve(reserveCount);
    DrainEmptySweptGlobal(maxRegions, reclaimed);

    if (reclaimed.size() > maxRegions) {
        for (size_t i = maxRegions; i < reclaimed.size(); ++i) {
            PushEmptySweptRegion(reclaimed[i]);
        }
        reclaimed.resize(maxRegions);
    }

    size_t promotedBytes = 0;
    std::vector<FreeManager::RegionSpan> spans;
    spans.reserve(reclaimed.size());
    for (MSRegionInfo* region : reclaimed) {
        size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
        CHECK_DETAIL(region->IsFreeRegion(),
                     "[MS-GC] ReclaimEmptyRegion: bad regionType=%u idx=%zu",
                     static_cast<unsigned>(region->GetRegionType()), regionIdx);
        region->Reset();
        spans.push_back({ static_cast<FreeManager::RegionIndex>(regionIdx), 1 });
        promotedBytes += MS_REGION_SIZE;
    }
    msRegionManager_->ReclaimReleasedRegionsToHotFreeBatch(spans);

    return promotedBytes;
}

bool CentralRegionPool::HasIdleSegmentCachedRegions(size_t startRegionIdx, size_t endRegionIdx) const
{
    if (startRegionIdx >= endRegionIdx || msRegionManager_ == nullptr) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(emptySweptLock_));
        bool found = false;
        emptySweptCache_.ForEach([&](MSRegionInfo* region) {
            if (found || region == nullptr) {
                return;
            }
            size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
            if (regionIdx >= startRegionIdx && regionIdx < endRegionIdx) {
                found = true;
            }
        });
        if (found) {
            return true;
        }
    }

    return false;
}

size_t CentralRegionPool::ReclaimIdleSegmentCaches(size_t startRegionIdx, size_t endRegionIdx)
{
    if (startRegionIdx >= endRegionIdx || msRegionManager_ == nullptr) {
        return 0;
    }

    std::vector<MSRegionInfo*> reclaimedEmptyRegions;
    {
        std::vector<MSRegionInfo*> kept;
        std::lock_guard<std::mutex> lg(emptySweptLock_);
        for (;;) {
            MSRegionInfo* region = emptySweptCache_.TakeHead();
            if (region == nullptr) {
                break;
            }
            size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
            if (regionIdx >= startRegionIdx && regionIdx < endRegionIdx) {
                reclaimedEmptyRegions.push_back(region);
                continue;
            }
            kept.push_back(region);
        }
        for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
            emptySweptCache_.Prepend(*it);
        }
    }
    if (!reclaimedEmptyRegions.empty()) {
        DecrementEmptySweptCount(reclaimedEmptyRegions.size());
    }

    size_t promotedBytes = 0;
    if (!reclaimedEmptyRegions.empty()) {
        std::vector<FreeManager::RegionSpan> spans;
        spans.reserve(reclaimedEmptyRegions.size());
        for (MSRegionInfo* region : reclaimedEmptyRegions) {
            size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
            CHECK_DETAIL(region->IsFreeRegion(),
                         "[MS-GC] ReclaimIdleSegmentCaches: bad empty-cache regionType=%u idx=%zu",
                         static_cast<unsigned>(region->GetRegionType()), regionIdx);
            region->Reset();
            spans.push_back({ static_cast<FreeManager::RegionIndex>(regionIdx), 1 });
            promotedBytes += MS_REGION_SIZE;
        }
        msRegionManager_->ReclaimReleasedRegionsToHotFreeBatch(spans);
    }

    return promotedBytes;
}

// ============================================================================
// GC operations
// ============================================================================

uint32_t CentralRegionPool::PrepareCollectSetForMarkUnderSTW(bool forceMaxHelpers)
{
    uint64_t prepareStartUs = TimeUtil::MicroSeconds();
    uint32_t previousEpoch = currentMarkEpoch_.load(std::memory_order_relaxed);
    uint32_t epoch = previousEpoch + 1;
    if (UNLIKELY(epoch == MSRegionInfo::NO_COLLECT_EPOCH)) {
        epoch = 1;
    }

    size_t collected = 0;
    size_t tlSmallCleared = 0;
    size_t largeBaseCleared = 0;
    size_t estimatedResetBytes = 0;
    struct ResetLane {
        RegionChain* chain;
        uint8_t cls;
        uint8_t scan;
    };
    std::array<ResetLane, kNumSizeClasses * kMSScanClassCount> resetLanes{};
    size_t resetLaneCount = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            RegionChain& chain = collectSet_[cls][scan];
            CHECK_DETAIL(chain.IsEmpty(),
                         "[MS-GC] PrepareCollectSetForMark: stale collect-set chain cls=%u scan=%u count=%zu",
                         static_cast<unsigned>(cls), static_cast<unsigned>(scan), chain.count);
            buckets_[cls][scan].StealSweptToCollectSet(chain);
            if (!chain.IsEmpty()) {
                resetLanes[resetLaneCount++] = {&chain, cls, scan};
                size_t bitmapBytes = static_cast<size_t>(SizeClassInfo::ClassToBitmapWords(cls)) *
                    sizeof(uint64_t);
                estimatedResetBytes += chain.count * bitmapBytes *
                    (scan == ToMSScanClassIndex(MSScanClass::HAS_REF) ? 2 : 1);
            }
            collected += chain.count;
        }
    }

    auto resetSmallLane = [epoch](RegionChain& chain, uint8_t cls, uint8_t scan) {
        chain.ForEach([epoch, cls, scan](MSRegionInfo* region) {
                CHECK_DETAIL(
                    region->GetRegionKind() == MSRegionKind::SMALL &&
                    region->GetAllocMode() == MSAllocMode::BITMAP &&
                    region->sizeClass == cls &&
                    ToMSScanClassIndex(region->GetScanClass()) == scan,
                    "[MS-GC] PrepareCollectSetForMark: invalid lane member idx=%zu cls=%u/%u scan=%u/%u "
                    "kind=%u mode=%u",
                    MSRegionInfoArray::GetRegionIdx(region),
                    static_cast<unsigned>(region->sizeClass),
                    static_cast<unsigned>(cls),
                    static_cast<unsigned>(ToMSScanClassIndex(region->GetScanClass())),
                    static_cast<unsigned>(scan),
                    static_cast<unsigned>(region->GetRegionKind()),
                    static_cast<unsigned>(region->GetAllocMode()));
                region->ResetSmallMarkWorkState();
                region->collectEpoch = epoch;
            });
    };

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    GCThreadPool* threadPool = resources.GetThreadPool();
    size_t availableHelpers = threadPool == nullptr ? 0 : std::min(
        {resetLaneCount, resources.GetMSGCThreadPoolHelperLimit(),
         static_cast<size_t>(threadPool->GetMaxThreadNum())});
    size_t helperCount = forceMaxHelpers
        ? availableHelpers
        : ComputeMSPreEnumResetHelperTarget(estimatedResetBytes, availableHelpers);
    bool parallelReset = helperCount != 0;
    bool preEnumPoolOwned = false;
    bool parallelResetStarted = false;
    int32_t oldActiveThreadCount = 0;

    if (parallelReset && resources.TryAcquireMSGCWorkerPoolForPreEnum()) {
        preEnumPoolOwned = true;
        std::array<std::unique_ptr<HeapWork>, kNumSizeClasses * kMSScanClassCount> stagedTasks{};
        bool allocationFailed = false;
        for (size_t laneIndex = 0; laneIndex < resetLaneCount; ++laneIndex) {
            const ResetLane lane = resetLanes[laneIndex];
            std::unique_ptr<HeapWork> task(new (std::nothrow) LambdaWork(
                [chain = lane.chain, cls = lane.cls, scan = lane.scan, resetSmallLane](size_t) {
                    resetSmallLane(*chain, cls, scan);
                }));
            if (task == nullptr) {
                allocationFailed = true;
                break;
            }
            stagedTasks[laneIndex] = std::move(task);
        }
        if (allocationFailed) {
            resources.ReleaseMSGCWorkerPool(MSGCWorkerPoolOwner::PRE_ENUM);
            preEnumPoolOwned = false;
            parallelReset = false;
        } else {
            oldActiveThreadCount = threadPool->GetMaxActiveThreadNum();
            threadPool->SetMaxActiveThreadNum(static_cast<int32_t>(helperCount));
            for (size_t laneIndex = 0; laneIndex < resetLaneCount; ++laneIndex) {
                threadPool->AddWork(stagedTasks[laneIndex].release());
            }
            threadPool->Start();
            parallelResetStarted = true;
        }
    } else {
        parallelReset = false;
    }

    if (!parallelResetStarted) {
        for (size_t laneIndex = 0; laneIndex < resetLaneCount; ++laneIndex) {
            const ResetLane lane = resetLanes[laneIndex];
            resetSmallLane(*lane.chain, lane.cls, lane.scan);
        }
    }

    // Freeze LARGE membership with one O(1) vector swap. Later allocations
    // append only to largeNextSupply_ and are outside this cycle.
    {
        std::lock_guard<std::mutex> lg(largeLock_);
        CHECK_DETAIL(
            largeCollectSet_.empty(),
            "[MS-GC] PrepareCollectSetForMark: stale LARGE collect-set entries=%zu",
            largeCollectSet_.size());
        largeCollectSet_.swap(largeNextSupply_);
        for (uint32_t idx : largeCollectSet_) {
            MSRegionInfo* region =
                MSRegionInfoArray::GetRegionInfoByIdx(idx);
            CHECK_DETAIL(
                region != nullptr &&
                region->GetRegionKind() == MSRegionKind::LARGE,
                "[MS-GC] PrepareCollectSetForMark: invalid LARGE primary idx=%u",
                idx);
            CHECK_DETAIL(
                region->GetMarkTaskNextRegionIdx() == MSRegionInfo::INVALID_REGION_IDX,
                "[MS-GC] LARGE Region still linked to ObjectReady at cycle reset: idx=%zu next=%u",
                MSRegionInfoArray::GetRegionIdx(region),
                region->GetMarkTaskNextRegionIdx());
            region->ClearMarkedFlag();
            region->collectEpoch = epoch;
            ++largeBaseCleared;
        }
    }

    // ACTIVE Regions are outside this collection boundary. Clear only their
    // MARK/SCAN task workspace; never materialize or read their live ALLOC
    // plane. They remain owned by the mutator across this cycle.
    VisitAllocBufferMS([&tlSmallCleared](AllocBufferMS& buffer) {
        for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
            for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
                MSRegionInfo* region =
                    buffer.GetActiveRegion(cls, static_cast<MSScanClass>(scan));
                if (region == nullptr) {
                    continue;
                }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
                CHECK_DETAIL(
                    region->GetRegionKind() == MSRegionKind::SMALL &&
                    (region->GetAllocMode() == MSAllocMode::BUMP ||
                     region->GetAllocMode() == MSAllocMode::BITMAP),
                    "[MS-GC] unexpected TLS Region type during mark bitmap clear: "
                    "idx=%zu cls=%u scan=%u type=%u",
                    MSRegionInfoArray::GetRegionIdx(region),
                    static_cast<unsigned>(cls),
                    static_cast<unsigned>(scan),
                    static_cast<unsigned>(region->GetRegionType()));
#endif
                region->collectEpoch = MSRegionInfo::NO_COLLECT_EPOCH;
                region->ResetSmallMarkWorkState();
                ++tlSmallCleared;
            }
        }
    });

    if (parallelResetStarted) {
        threadPool->WaitFinish();
        threadPool->SetMaxActiveThreadNum(oldActiveThreadCount);
    }
    if (preEnumPoolOwned) {
        resources.ReleaseMSGCWorkerPool(MSGCWorkerPoolOwner::PRE_ENUM);
    }

    // Do not expose the new mark epoch until all mark state is reset. Mutators
    // are stopped here, and barrier buffers were reset before this method.
    currentMarkEpoch_.store(epoch, std::memory_order_release);
    PublishMSGCFastPathMarkEpoch(epoch);

    VLOG(REPORT,
         "[MS-GC] PrepareCollectSetForMark: epoch=%u collectSetSmall=%zu tlSmall=%zu "
         "largeBase=%zu resetBytes=%zu resetLanes=%zu helpers=%zu parallelReset=%d elapsed=%sus",
         static_cast<unsigned>(epoch), collected, tlSmallCleared, largeBaseCleared,
         estimatedResetBytes, resetLaneCount, helperCount, static_cast<int>(parallelReset),
         Pretty(TimeUtil::MicroSeconds() - prepareStartUs).Str());
    return epoch;
}

size_t CentralRegionPool::GetCollectSetRegionCount() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            total += collectSet_[cls][scan].count;
        }
    }
    return total;
}

void CentralRegionPool::PublishCollectSetForSweep()
{
    size_t totalUnswept = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            RegionChain& chain = collectSet_[cls][scan];
            totalUnswept += chain.count;
            buckets_[cls][scan].PublishCollectSetToUnswept(chain);
            CHECK(chain.IsEmpty());
        }
    }
    centralSmallUnsweptRegions_.store(totalUnswept, std::memory_order_release);

    forceHighSweepPressure_.store(false, std::memory_order_relaxed);
    sweepPressureMode_.store(
        static_cast<int>(SweepPressureMode::LOW), std::memory_order_relaxed);
    sweepCredit_.Reset();
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    size_t runwayRegions = ComputeRunwayRegions(
        space.GetHeapThresholdBytes(), space.GetSweptHeapBytes(), MS_REGION_SIZE);
    bool disableAssist = ReadBoolParameter("CJ_MS_DISABLE_ASSIST", false);
    uint64_t sweepAssistRatio = disableAssist
        ? 0
        : ComputeMSSweepAssistRatioScaled(totalUnswept, runwayRegions);
    sweepRegionsPerAllocScaled_.store(
        sweepAssistRatio, std::memory_order_relaxed);
    uint32_t newSweepCycleId =
        sweepCycleId_.fetch_add(1, std::memory_order_acq_rel) + 1;
    MSRegionManager* regionManager = space.GetMSRegionManager();
    if (regionManager != nullptr) {
        regionManager->AdvanceScavengeEpoch(newSweepCycleId);
    }
    sweepDone_.store(totalUnswept == 0, std::memory_order_release);

    VLOG(REPORT,
         "[MS-GC] PublishCollectSetForSweep: epoch=%u unsweptRegions=%zu "
         "mutatorAssistDisabled=%d assistRatio=%llu",
         static_cast<unsigned>(
             currentMarkEpoch_.load(std::memory_order_relaxed)),
         totalUnswept,
         static_cast<int>(disableAssist),
         static_cast<unsigned long long>(sweepAssistRatio));
}

// ============================================================================
// BackgroundSweep — called by gc-helper thread (BackgroundProcessor)
// ============================================================================

void CentralRegionPool::BackgroundSweep(uint8_t preferredCls, uint64_t deadlineUs)
{
    if (!TryEnterSweep()) {
        return;
    }
    bgSweepRunning_.store(true, std::memory_order_release);
    auto finish = [this]() {
        bgSweepRunning_.store(false, std::memory_order_release);
        LeaveSweep();
    };

    if (stopSweepRequested_.load(std::memory_order_acquire)) {
        finish();
        return;
    }

    size_t remainingUnswept = GetTotalUnsweptCount();
    if (remainingUnswept == 0) {
        finish();
        return;
    }
    size_t sweptInSlice = 0;

    auto SweepBucket = [&](uint8_t cls) -> bool {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            SizeClassBucket& bucket = buckets_[cls][scan];
            if (!SweepUnsweptList(
                bucket, sweptInSlice, SIZE_MAX, deadlineUs)) {
                return false;
            }
        }
        return true;
    };

    auto classOrder = BuildBackgroundSweepClassOrder(preferredCls);
    for (uint8_t cls : classOrder) {
        if (!SweepBucket(cls)) {
            break;
        }
    }

    RecordBackgroundSweep(sweptInSlice);
    finish();
}

void CentralRegionPool::ParallelSweep(uint8_t preferredCls, uint64_t deadlineUs)
{
    if (!TryEnterSweep()) {
        return;
    }
    if (skipHelperSweep_.load(std::memory_order_acquire) ||
        stopSweepRequested_.load(std::memory_order_acquire)) {
        LeaveSweep();
        return;
    }

    size_t sweptInSlice = 0;
    auto classOrder = BuildMSSweepCyclicClassOrder<kNumSizeClasses>(preferredCls);
    for (uint8_t cls : classOrder) {
        bool completedClass = true;
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            SizeClassBucket& bucket = buckets_[cls][scan];
            if (!SweepUnsweptList(
                bucket, sweptInSlice, SIZE_MAX, deadlineUs)) {
                completedClass = false;
                break;
            }
        }
        if (!completedClass) {
            break;
        }
    }

    RecordHelperSweep(sweptInSlice);
    LeaveSweep();
}

void CentralRegionPool::ParallelSweepOnceForOOM(uint8_t preferredCls)
{
    // PreEnumRoots closed every ordinary Sweep lane before Mark. OOM PostMark
    // deliberately keeps them closed while the GC main thread and all configured
    // Sweep helpers claim disjoint Regions from the same unswept head.
    CHECK(stopSweepRequested_.load(std::memory_order_acquire));
    CHECK(skipAssistSweep_.load(std::memory_order_acquire));
    CHECK(skipHelperSweep_.load(std::memory_order_acquire));

    size_t sweptInPass = 0;
    auto classOrder = BuildMSSweepCyclicClassOrder<kNumSizeClasses>(preferredCls);
    for (uint8_t cls : classOrder) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            SizeClassBucket& bucket = buckets_[cls][scan];
            (void)SweepUnsweptList(
                bucket, sweptInPass, SIZE_MAX, UINT64_MAX);
        }
    }
}

MSSweepAssistResult CentralRegionPool::MSSweepAssist(size_t debtRegions, uint8_t preferredCls)
{
    MSSweepAssistResult result;
    if (debtRegions == 0 || sweepDone_.load(std::memory_order_acquire) ||
        GetTotalUnsweptCount() == 0) {
        result.sweepComplete = sweepDone_.load(std::memory_order_acquire);
        return result;
    }

    // Background/lazy/helper work is a one-shot global credit. CAS claiming is
    // the ownership transfer: once one P consumes it, no other P can use the
    // same swept Region to reduce its local debt.
    result.creditPaidRegions = sweepCredit_.Take(debtRegions);
    size_t remainingDebt = debtRegions - result.creditPaidRegions;
    if (remainingDebt == 0) {
        result.sweepComplete = sweepDone_.load(std::memory_order_acquire);
        return result;
    }

    // Pressure scales debt addition, never repayment speed. Retain a sub-batch
    // remainder; once it reaches the configured threshold, run one exact batch.
    size_t toSweep = ComputeMSSweepAssistDirectRegions(
        remainingDebt, CangjieRuntime::GetGCParam().msAssistSweepMaxRegionsPerCall);
    if (toSweep == 0) {
        result.sweepComplete = sweepDone_.load(std::memory_order_acquire);
        return result;
    }

    // AssistSweep carries the unified admission guard. GC closes that guard
    // before clearing mark bitmaps, so no new SweepRegion can start afterward.
    result.assistPaidRegions = AssistSweep(toSweep, preferredCls);
    result.sweepComplete = sweepDone_.load(std::memory_order_acquire);
    return result;
}

SweepPressureMode CentralRegionPool::RefreshSweepDebtSnapshot(size_t thresholdBytes, size_t sweptBytes)
{
    size_t unsweptBytes = GetTotalUnsweptBytes();
    bool forcedHigh = forceHighSweepPressure_.load(std::memory_order_acquire);
    SweepPressureMode previousMode = GetSweepPressureMode();
    SweepPressureMode mode = sweepDone_.load(std::memory_order_acquire) || unsweptBytes == 0
        ? SweepPressureMode::LOW
        : EvaluateSweepPressure(unsweptBytes, sweptBytes, thresholdBytes, forcedHigh, previousMode);
    if (unsweptBytes == 0) {
        forceHighSweepPressure_.store(false, std::memory_order_release);
    }
    sweepPressureMode_.store(static_cast<int>(mode), std::memory_order_release);

    return mode;
}

void CentralRegionPool::ForceHighSweepPressure()
{
    if (sweepDone_.load(std::memory_order_acquire) || GetTotalUnsweptCount() == 0) {
        return;
    }
    forceHighSweepPressure_.store(true, std::memory_order_release);
    sweepPressureMode_.store(static_cast<int>(SweepPressureMode::HIGH), std::memory_order_release);
    // Close the race where the final Sweep publisher completed immediately
    // before these stores. A completed epoch must never retain forced HIGH.
    if (sweepDone_.load(std::memory_order_acquire)) {
        forceHighSweepPressure_.store(false, std::memory_order_release);
        sweepPressureMode_.store(static_cast<int>(SweepPressureMode::LOW), std::memory_order_release);
    }
}

size_t CentralRegionPool::AssistSweep(size_t targetRegions, uint8_t preferredCls)
{
    if (targetRegions == 0) {
        return 0;
    }

    if (!TryEnterSweep()) {
        return 0;
    }
    if (skipAssistSweep_.load(std::memory_order_acquire) ||
        stopSweepRequested_.load(std::memory_order_acquire)) {
        LeaveSweep();
        return 0;
    }

    size_t sweptRegions = 0;
    auto classOrder = BuildMSSweepCyclicClassOrder<kNumSizeClasses>(preferredCls);
    for (uint8_t cls : classOrder) {
        for (uint8_t scan = 0;
             scan < kMSScanClassCount && sweptRegions < targetRegions;
             ++scan) {
            SizeClassBucket& bucket = buckets_[cls][scan];
            while (sweptRegions < targetRegions &&
                   SweepOneUnsweptRegionForAssist(bucket)) {
                ++sweptRegions;
            }
        }
        if (sweptRegions >= targetRegions) {
            break;
        }
    }
    LeaveSweep();
    return sweptRegions;
}

bool CentralRegionPool::SweepOneUnsweptRegionForAssist(
    SizeClassBucket& bucket, size_t* emptiedInSlice)
{
    MSRegionInfo* region = bucket.ClaimUnswept();
    if (region == nullptr) {
        return false;
    }

    MSCentralReturnDecision decision = region->SweepRegion();
    region->collectEpoch = MSRegionInfo::NO_COLLECT_EPOCH;
    if (decision.kind == MSCentralReturnKind::EMPTY_CACHE) {
        PushEmptySweptRegion(region);
        if (emptiedInSlice != nullptr) {
            (*emptiedInSlice)++;
        }
    } else if (decision.kind == MSCentralReturnKind::PARTIAL) {
        bucket.PushPartialSwept(region);
    } else {
        bucket.PushFullSwept(region);
    }
    return true;
}

bool CentralRegionPool::SweepUnsweptList(
    SizeClassBucket& bucket, size_t& sweptInSlice, size_t maxRegions,
    uint64_t deadlineUs, size_t* emptiedInSlice)
{
    for (;;) {
        if (sweptInSlice >= maxRegions) {
            return false;
        }
        if (sweptInSlice % kSweepDeadlineCheckInterval == 0 &&
            TimeUtil::MicroSeconds() >= deadlineUs) {
            return false;
        }

        MSRegionInfo* region = bucket.ClaimUnswept();
        if (region == nullptr) {
            break;
        }

        MSCentralReturnDecision decision = region->SweepRegion();
        region->collectEpoch = MSRegionInfo::NO_COLLECT_EPOCH;
        sweptInSlice++;

        if (decision.kind == MSCentralReturnKind::EMPTY_CACHE) {
            // Completely empty regions leave the class-local supply and
            // become classless empty-cache entries.
            PushEmptySweptRegion(region);
            if (emptiedInSlice != nullptr) {
                (*emptiedInSlice)++;
            }
        } else if (decision.kind == MSCentralReturnKind::PARTIAL) {
            bucket.PushPartialSwept(region);
        } else {
            bucket.PushFullSwept(region);
        }

    }

    return true;
}

// ===== Monitoring =====

size_t CentralRegionPool::GetTotalUnsweptCount() const
{
    return centralSmallUnsweptRegions_.load(std::memory_order_acquire);
}

size_t CentralRegionPool::GetTotalUnsweptCountReference() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            total += buckets_[cls][scan].CountUnswept();
        }
    }
    return total;
}

size_t CentralRegionPool::GetTotalUnsweptBytes() const
{
    uint64_t bytes = SaturatingMultiplyMSSweepValue(
        static_cast<uint64_t>(GetTotalUnsweptCount()), static_cast<uint64_t>(MS_REGION_SIZE));
    return bytes > std::numeric_limits<size_t>::max()
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(bytes);
}

size_t CentralRegionPool::GetLargeRegionCount() const
{
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(largeLock_));
    return largeCollectSet_.size() + largeNextSupply_.size();
}

size_t CentralRegionPool::GetSmallSweptRegionCount() const
{
    return centralSmallSweptRegions_.load(std::memory_order_acquire);
}

size_t CentralRegionPool::GetLargeNextSupplySpanCount() const
{
    size_t spanCount = 0;
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(largeLock_));
    for (uint32_t idx : largeNextSupply_) {
        MSRegionInfo* region = MSRegionInfoArray::GetRegionInfoByIdx(idx);
        CHECK_DETAIL(
            region != nullptr && region->GetRegionKind() == MSRegionKind::LARGE,
            "[MS-GC] GetLargeNextSupplySpanCount: invalid LARGE primary idx=%u",
            idx);
        size_t regionSpanCount = region->LargeSpanCount();
        spanCount = regionSpanCount > std::numeric_limits<size_t>::max() - spanCount
            ? std::numeric_limits<size_t>::max()
            : spanCount + regionSpanCount;
    }
    return spanCount;
}

void CentralRegionPool::ForceFinishUnsweptList(
    SizeClassBucket& bucket, size_t& sweptRegions, size_t& emptiedRegions,
    size_t& retainedRegions, size_t& freedBytes)
{
    for (;;) {
        MSRegionInfo* region = bucket.ClaimUnswept();
        if (region == nullptr) {
            break;
        }

        MSCentralReturnDecision decision = region->SweepRegion();
        region->collectEpoch = MSRegionInfo::NO_COLLECT_EPOCH;
        sweptRegions++;
        freedBytes += static_cast<size_t>(decision.freeSlots) *
            region->GetSlotSize();

        if (decision.kind == MSCentralReturnKind::EMPTY_CACHE) {
            emptiedRegions++;
            PushEmptySweptRegion(region);
            continue;
        }

        retainedRegions++;
        if (decision.kind == MSCentralReturnKind::PARTIAL) {
            bucket.PushPartialSwept(region);
            continue;
        }

        bucket.PushFullSwept(region);
    }
}

void CentralRegionPool::ForceFinishAllUnswept()
{
    size_t sweptRegions = 0;
    size_t emptiedRegions = 0;
    size_t retainedRegions = 0;
    size_t freedBytes = 0;

    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            SizeClassBucket& bucket = buckets_[cls][scan];
            ForceFinishUnsweptList(
                bucket, sweptRegions, emptiedRegions, retainedRegions, freedBytes);
        }
    }

    VLOG(REPORT,
        "[MS-GC] ForceFinishAllUnswept: swept=%zu regions, emptied=%zu, retained=%zu, freed=%s B",
        sweptRegions, emptiedRegions, retainedRegions, Pretty(freedBytes).Str());
}

void CentralRegionPool::ForceFinishAllUnsweptForOOM()
{
    // PreEnumRoots closed these gates before Mark and the OOM path deliberately
    // did not reopen them in MarkTermination. Therefore GC main is the only
    // thread that can claim a Region from this cycle's unswept lists here.
    CHECK(stopSweepRequested_.load(std::memory_order_acquire));
    CHECK(skipAssistSweep_.load(std::memory_order_acquire));
    CHECK(skipHelperSweep_.load(std::memory_order_acquire));

    forceFinishSweepRunning_.store(true, std::memory_order_release);
    size_t unsweptBefore = GetTotalUnsweptCount();
    ForceFinishAllUnswept();
    CHECK(GetTotalUnsweptCount() == 0);
    CHECK(GetTotalUnsweptCountReference() == 0);
    forceFinishSweepRunning_.store(false, std::memory_order_release);
    // OOM force-finish is an emergency allocation path, not normal completion
    // of this sweep epoch. Keep sweepDone_ unchanged; only the normal GC
    // StopAndWaitForAllSweep path may publish it.
    forceHighSweepPressure_.store(false, std::memory_order_release);
    sweepPressureMode_.store(static_cast<int>(SweepPressureMode::LOW), std::memory_order_release);
    VLOG(REPORT,
         "[MS-GC] OOM force sweep: unswept=%zu->0 emptyCacheRegions=%zu",
         unsweptBefore, GetEmptySweptRegionCount());
}

size_t CentralRegionPool::CountTLActiveRegions() const
{
    size_t count = 0;
    // VisitAllocBufferMS requires non-const; safe to const_cast for read-only counting
    const_cast<CentralRegionPool*>(this)->VisitAllocBufferMS([&count](AllocBufferMS& buf) {
        for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
            for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
                if (buf.GetActiveRegion(cls, static_cast<MSScanClass>(scan)) != nullptr) {
                    count++;
                }
            }
        }
    });
    return count;
}

size_t CentralRegionPool::GetAllocBufferCount() const
{
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
    return msAllocBuffers_.size();
}

size_t CentralRegionPool::GetAllAllocBufferActiveRegionCount() const
{
    size_t total = 0;
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
    for (AllocBufferMS* buffer : msAllocBuffers_) {
        if (buffer != nullptr) {
            total += buffer->CountActiveRegions();
        }
    }
    return total;
}

size_t CentralRegionPool::GetSweptMSRegionCount() const
{
    return centralSmallSweptRegions_.load(std::memory_order_relaxed) +
           largeLiveSpanCount_.load(std::memory_order_relaxed);
}

size_t CentralRegionPool::GetSweptMSRegionCountReference() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            total += buckets_[cls][scan].CountPartialSwept();
            total += buckets_[cls][scan].CountFullSwept();
        }
    }
    total += largeLiveSpanCount_.load(std::memory_order_acquire);
    return total;
}

size_t CentralRegionPool::GetActiveMSRegionCount() const
{
    return centralSmallActiveRegions_.load(std::memory_order_relaxed) +
           largeLiveSpanCount_.load(std::memory_order_relaxed);
}

size_t CentralRegionPool::GetActiveMSRegionCountReference() const
{
    size_t total = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            total += buckets_[cls][scan].CountPartialSwept();
            total += buckets_[cls][scan].CountFullSwept();
            total += buckets_[cls][scan].CountUnswept();
        }
    }
    total += largeLiveSpanCount_.load(std::memory_order_acquire);
    return total;
}

size_t CentralRegionPool::GetEmptySweptRegionCount() const
{
    return emptySweptCount_.load(std::memory_order_acquire);
}

CentralRegionPool::BucketPressure CentralRegionPool::GetBucketPressure(uint8_t cls) const
{
    BucketPressure total;
    for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
        BucketPressure lane = buckets_[cls][scan].GetPressure();
        total.partialSwept += lane.partialSwept;
        total.fullSwept += lane.fullSwept;
        total.unswept += lane.unswept;
    }
    return total;
}

void CentralRegionPool::DumpPoolStats() const
{
    size_t partialSweptTotal = 0;
    size_t fullSweptTotal = 0;
    size_t unsweptTotal = 0;
    size_t emptySweptTotal = GetEmptySweptRegionCount();

    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            partialSweptTotal += buckets_[cls][scan].CountPartialSwept();
            fullSweptTotal += buckets_[cls][scan].CountFullSwept();
            unsweptTotal += buckets_[cls][scan].CountUnswept();
        }
    }

    size_t totalSmall =
        partialSweptTotal + fullSweptTotal + unsweptTotal + emptySweptTotal;

    VLOG(REPORT, "[MS-GC]   CentralPool small (%u classes aggregated):", kNumSizeClasses);
    VLOG(REPORT, "[MS-GC]     partialSwept: %zu regions  fullSwept: %zu regions",
         partialSweptTotal, fullSweptTotal);
    VLOG(REPORT, "[MS-GC]     unswept: %zu regions", unsweptTotal);
    VLOG(REPORT, "[MS-GC]     emptySwept(global+local): %zu regions", emptySweptTotal);
    VLOG(REPORT, "[MS-GC]     total small: %zu regions", totalSmall);
    VLOG(REPORT, "[MS-GC]     large regions: %zu", GetLargeRegionCount());
}

// ============================================================================
// Large Region management (Tier 1 in-heap large objects)
// ============================================================================

void CentralRegionPool::AddLargeLiveRegion(MSRegionInfo* region)
{
    size_t spanCount = region->LargeSpanCount();
    std::lock_guard<std::mutex> lg(largeLock_);
    largeNextSupply_.push_back(static_cast<uint32_t>(
        MSRegionInfoArray::GetRegionIdx(region)));
    largeLiveSpanCount_.fetch_add(spanCount, std::memory_order_release);
    largeLiveObjectBytes_.fetch_add(region->LargeObjectSize(), std::memory_order_release);
}

void CentralRegionPool::RegisterLargeRegion(MSRegionInfo* region)
{
    AddLargeLiveRegion(region);
}

void CentralRegionPool::CollectDeadLargeRegions(
    uint32_t collectEpoch, std::vector<MSRegionInfo*>& deadRegions)
{
    std::lock_guard<std::mutex> lg(largeLock_);
    for (uint32_t idx : largeCollectSet_) {
        MSRegionInfo* region =
            MSRegionInfoArray::GetRegionInfoByIdx(idx);
        CHECK_DETAIL(
            region != nullptr &&
            region->GetRegionKind() == MSRegionKind::LARGE,
            "[MS-GC] CollectDeadLargeRegions: invalid primary idx=%u",
            idx);
        if (region->collectEpoch == collectEpoch && !region->IsMarked()) {
            size_t spanCount = region->LargeSpanCount();
            largeLiveSpanCount_.fetch_sub(
                spanCount, std::memory_order_release);
            largeLiveObjectBytes_.fetch_sub(
                region->LargeObjectSize(), std::memory_order_release);
            deadRegions.push_back(region);
        } else {
            largeNextSupply_.push_back(idx);
        }
    }
    largeCollectSet_.clear();
}

void CentralRegionPool::SweepLargeRegions()
{
    // Two-pass to avoid holding largeLock_ during ReclaimRegion.
    std::vector<MSRegionInfo*> deadRegions;

    CollectDeadLargeRegions(
        currentMarkEpoch_.load(std::memory_order_acquire), deadRegions);

    // Release dead large regions
    for (MSRegionInfo* msInfo : deadRegions) {
        uintptr_t regionStart = msInfo->LargeObjectStart();
        size_t regionBytes = msInfo->LargeSpanBytes();
        size_t spanCount = msInfo->LargeSpanCount();

        // Reset span MSRegionInfo entries before resetting the base entry.
        // RegisterLargeObjectMS initializes secondary entries as MS_REGION_SPAN
        // and reserved=baseIdx; we must clear them to avoid stale lookups.
        size_t baseIdx = MSRegionInfoArray::GetRegionIdx(msInfo);
        for (size_t i = 1; i < spanCount; i++) {
            MSRegionInfo* spanInfo = MSRegionInfoArray::GetRegionInfoByIdx(baseIdx + i);
            spanInfo->Reset();
        }

        // Reset base MSRegionInfo before publishing the whole span as free.
        msInfo->Reset();
        msRegionManager_->ReclaimRegionLarge(regionStart, regionBytes);
    }
}

// ============================================================================
// Per-thread AllocBufferMS registry
// ============================================================================
void CentralRegionPool::RegisterAllocBufferMS(AllocBufferMS* buffer)
{
    std::lock_guard<std::mutex> lg(msAllocBufferLock_);
    msAllocBuffers_.push_back(buffer);
}

void CentralRegionPool::RemoveAllocBufferMS(AllocBufferMS* buffer)
{
    std::lock_guard<std::mutex> lg(msAllocBufferLock_);
    auto it = std::find(msAllocBuffers_.begin(), msAllocBuffers_.end(), buffer);
    if (it != msAllocBuffers_.end()) {
        msAllocBuffers_.erase(it);
    }
}

// ============================================================================
// ComputeMarkedStats: walk frozen collect-set markBitmaps once
// ============================================================================
size_t CentralRegionPool::GetCollectSetBitmapBytes() const
{
    size_t bitmapBytes = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        size_t bytesPerRegion =
            static_cast<size_t>(SizeClassInfo::ClassToBitmapWords(cls)) *
            sizeof(uint64_t);
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            bitmapBytes += collectSet_[cls][scan].count * bytesPerRegion;
        }
    }
    return bitmapBytes;
}

MSMarkedStats CentralRegionPool::ComputeMarkedStats(
    GCThreadPool* threadPool, size_t helperCount) const
{
    constexpr size_t kMSPostMarkStatsRegionsPerChunk = 256;
    MSMarkedStats stats;

    auto countRegion = [](MSRegionInfo* region, uint8_t cls, uint8_t scan,
                          MSMarkedStats& target) {
        const size_t slotSize = SizeClassInfo::ClassToSize(cls);
        const size_t wordCount = SizeClassInfo::ClassToBitmapWords(cls);
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        CHECK_DETAIL(
            region->GetRegionKind() == MSRegionKind::SMALL &&
            region->GetAllocMode() == MSAllocMode::BITMAP &&
            region->sizeClass == cls &&
            ToMSScanClassIndex(region->GetScanClass()) == scan,
            "[MS-GC] ComputeMarkedStats: invalid collect-set lane member "
            "idx=%zu cls=%u/%u scan=%u/%u kind=%u mode=%u",
            MSRegionInfoArray::GetRegionIdx(region),
            static_cast<unsigned>(region->sizeClass),
            static_cast<unsigned>(cls),
            static_cast<unsigned>(
                ToMSScanClassIndex(region->GetScanClass())),
            static_cast<unsigned>(scan),
            static_cast<unsigned>(region->GetRegionKind()),
            static_cast<unsigned>(region->GetAllocMode()));
#endif
        const uint64_t* markBitmap = region->GetMarkBitmap();
        size_t marked = 0;
        for (size_t word = 0; word < wordCount; ++word) {
            marked += __builtin_popcountll(markBitmap[word]);
        }
        target.AddSlots(marked, slotSize);
    };

    helperCount = threadPool == nullptr ? 0 : std::min(
        {helperCount, kMSPostMarkStatsMaxHelpers,
         static_cast<size_t>(threadPool->GetMaxThreadNum())});

    struct StatsChunk {
        MSRegionInfo* head;
        size_t count;
        uint8_t cls;
        uint8_t scan;
    };
    std::vector<StatsChunk> chunks;
    if (helperCount != 0) {
        size_t regionCount = GetCollectSetRegionCount();
        chunks.reserve(
            regionCount / kMSPostMarkStatsRegionsPerChunk +
            kNumSizeClasses * kMSScanClassCount);
        for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
            for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
                const RegionChain& chain = collectSet_[cls][scan];
                int32_t idx = chain.headIdx;
                while (idx != MSRegionInfo::INVALID_IDX) {
                    MSRegionInfo* head =
                        MSRegionInfoArray::GetRegionInfoByIdx(idx);
                    size_t count = 0;
                    while (idx != MSRegionInfo::INVALID_IDX &&
                           count < kMSPostMarkStatsRegionsPerChunk) {
                        MSRegionInfo* region =
                            MSRegionInfoArray::GetRegionInfoByIdx(idx);
                        idx = static_cast<int32_t>(
                            region->GetCentralNextRegionIdx());
                        ++count;
                    }
                    chunks.push_back({head, count, cls, scan});
                }
            }
        }
        helperCount = chunks.empty() ? 0 : std::min(
            helperCount, chunks.size() - 1);
    }

    if (helperCount == 0 && chunks.empty()) {
        for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
            for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
                collectSet_[cls][scan].ForEach(
                    [cls, scan, &stats, &countRegion](MSRegionInfo* region) {
                        countRegion(region, cls, scan, stats);
                    });
            }
        }
    }

    auto countChunk = [&countRegion](const StatsChunk& chunk,
                                     MSMarkedStats& target) {
        MSRegionInfo* region = chunk.head;
        for (size_t index = 0; index < chunk.count; ++index) {
            CHECK_DETAIL(region != nullptr,
                         "[MS-GC] truncated marked-stats chunk");
            int32_t nextIdx = static_cast<int32_t>(
                region->GetCentralNextRegionIdx());
            MSRegionInfo* next = nextIdx == MSRegionInfo::INVALID_IDX
                ? nullptr
                : MSRegionInfoArray::GetRegionInfoByIdx(nextIdx);
            countRegion(region, chunk.cls, chunk.scan, target);
            region = next;
        }
    };

    bool allocationFallback = false;
    int32_t oldActiveThreadCount = 0;
    std::array<MSMarkedStats, kMSPostMarkStatsMaxHelpers + 1>
        participantStats{};
    std::atomic<size_t> nextChunk{0};
    auto claimChunks = [&chunks, &nextChunk, &countChunk](
                           MSMarkedStats& target) {
        for (;;) {
            size_t chunkIndex =
                nextChunk.fetch_add(1, std::memory_order_relaxed);
            if (chunkIndex >= chunks.size()) {
                return;
            }
            countChunk(chunks[chunkIndex], target);
        }
    };
    std::array<std::unique_ptr<HeapWork>, kMSPostMarkStatsMaxHelpers>
        stagedTasks{};
    for (size_t helper = 0; helper < helperCount; ++helper) {
        stagedTasks[helper].reset(new (std::nothrow) LambdaWork(
            [&claimChunks, &participantStats, helper](size_t) {
                claimChunks(participantStats[helper]);
            }));
        if (stagedTasks[helper] == nullptr) {
            allocationFallback = true;
            helperCount = 0;
            stagedTasks = {};
            break;
        }
    }

    if (helperCount != 0) {
        oldActiveThreadCount = threadPool->GetMaxActiveThreadNum();
        threadPool->SetMaxActiveThreadNum(
            static_cast<int32_t>(helperCount));
        for (size_t helper = 0; helper < helperCount; ++helper) {
            threadPool->AddWork(stagedTasks[helper].release());
        }
        threadPool->Start();
    }

    // GC main participates directly instead of waiting for the pool. Chunking
    // prevents one dominant size-class lane from serializing the whole scan.
    if (!chunks.empty()) {
        claimChunks(participantStats[kMSPostMarkStatsMaxHelpers]);
    }
    if (helperCount != 0) {
        threadPool->WaitFinish();
        threadPool->SetMaxActiveThreadNum(oldActiveThreadCount);
    }

    if (!chunks.empty()) {
        for (const MSMarkedStats& participant : participantStats) {
            stats.objectCount += participant.objectCount;
            stats.liveBytes += participant.liveBytes;
        }
    }

    // LARGE membership is typically small and protected by one mutex; keep it
    // serial after the bandwidth-heavy Small bitmap lanes have joined.
    VisitLargeCollectSetLocked([&stats](MSRegionInfo* region) {
        if (region->IsMarked()) {
            stats.AddLargeObject(region->LargeSpanBytes());
        }
    });

    VLOG(REPORT,
         "[MS-GC] ComputeMarkedStats: bitmapBytes=%zu helpers=%zu "
         "parallel=%d allocationFallback=%d",
         GetCollectSetBitmapBytes(), helperCount,
         static_cast<int>(helperCount != 0),
         static_cast<int>(allocationFallback));

    return stats;
}

// ============================================================================
// VisitMarkedObjects: walk markBitmaps and call func() on every marked object.
// ============================================================================
// Same validity window as ComputeMarkedStats: marking complete, no sweep
// started. Used by the mark-completeness verifier to inspect every live object.
//
void CentralRegionPool::VisitMarkedObjects(const std::function<void(BaseObject*)>& func) const
{
    // Walk one region's markBitmap, emitting each marked slot as a BaseObject*.
    auto visitRegion = [&func](MSRegionInfo* region, size_t slotSize) {
        if (region->GetRegionKind() != MSRegionKind::SMALL ||
            region->GetTotalSlotCount() == 0) {
            return;
        }
        uint64_t* markBitmap = region->GetMarkBitmap();
        uintptr_t base = region->RegionStart();
        size_t wordCount = region->GetBitmapWordCount();
        for (size_t w = 0; w < wordCount; w++) {
            uint64_t bits = markBitmap[w];
            while (bits != 0) {
                size_t bit = static_cast<size_t>(__builtin_ctzll(bits));
                bits &= (bits - 1);
                size_t slotIdx = w * 64 + bit;
                uintptr_t slotAddr = base + slotIdx * slotSize;
                func(reinterpret_cast<BaseObject*>(slotAddr));
            }
        }
    };

    // 1. Immutable collect-set plus OUTSIDE_SET central supply.
    for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
        size_t slotSize = SizeClassInfo::ClassToSize(cls);
        auto countRegion = [&visitRegion, slotSize](MSRegionInfo* region) {
            visitRegion(region, slotSize);
        };
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            const SizeClassBucket& bucket = buckets_[cls][scan];
            collectSet_[cls][scan].ForEach(countRegion);
            bucket.VisitPartialSweptUnsafe(countRegion);
            bucket.VisitNonPartialUnsafe(countRegion, countRegion);
        }
    }

    // 2. Thread-local AllocBufferMS active regions.
    {
        std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
        for (AllocBufferMS* buffer : msAllocBuffers_) {
            if (buffer == nullptr) {
                continue;
            }
            for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
                for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
                    MSRegionInfo* region =
                        buffer->GetActiveRegion(cls, static_cast<MSScanClass>(scan));
                    if (region == nullptr) {
                        continue;
                    }
                    visitRegion(region, SizeClassInfo::ClassToSize(cls));
                }
            }
        }
    }

    // 3. Live large regions.
    {
        VisitLargeRegionsLocked([&func](MSRegionInfo* region) {
            if (region->IsMarked()) {
                func(reinterpret_cast<BaseObject*>(region->LargeObjectStart()));
            }
        });
    }
}

// ============================================================================
// Pre-STW MarkSetup: Stop all sweeps, clear markBitmaps, reclassify lists
// ============================================================================

void CentralRegionPool::StopAndWaitForAllSweep()
{
    // The GC main thread participates as an uncounted Sweep claimant below.
    // Keep completion closed across both drains and the intervening quiescence
    // waits so another executor cannot publish done after GC main claims the
    // final Region but before it finishes SweepRegion().
    forceFinishSweepRunning_.store(true, std::memory_order_release);
    size_t unsweptBefore = GetTotalUnsweptCount();

    // 1. Join the still-open Sweep epoch as another claimant. Every Sweep
    // source uses ClaimUnsweptHeadForSweep(), so the GC thread can drain
    // different Regions concurrently with background/assist/helper work.
    // This pass deliberately does not publish sweepDone_: claimed Regions may
    // still be inside SweepRegion on another thread.
    ForceFinishAllUnswept();

    // 2. Close all Sweep admission paths before establishing quiescence. The
    // unified admission word makes closing and active-count accounting one
    // atomic protocol; these flags remain as lane-specific fast exits.
    stopSweepRequested_.store(true, std::memory_order_release);
    skipAssistSweep_.store(true, std::memory_order_release);
    skipHelperSweep_.store(true, std::memory_order_release);

    // 3. Wait for every admitted sweep source. A worker that starts after this
    // close cannot increment the packed active count.
    CloseSweepAdmissionAndWait();

    // 4. Drain anything left unclaimed while admission was being
    // closed. With every in-flight source quiescent, this is the final serial
    // closure before mark bitmap clearing.
    ForceFinishAllUnswept();
    CHECK(GetTotalUnsweptCount() == 0);
    CHECK(GetTotalUnsweptCountReference() == 0);
    forceFinishSweepRunning_.store(false, std::memory_order_release);
    PublishSweepDoneAfterQuiescence();
    CHECK(IsSweepDone());

    size_t unsweptAfter = GetTotalUnsweptCount();
    size_t activeBytes = GetActiveMSRegionCount() * MS_REGION_SIZE;
    size_t emptySwept = GetEmptySweptRegionCount() * MS_REGION_SIZE;
    size_t totalRetained = activeBytes + emptySwept;
    VLOG(REPORT, "[MS-GC] StopAndWaitForAllSweep: forceSwept=%zu->%zu unswept regions, "
         "retained=%zuB activeBytes=%zuB emptySwept=%zuB",
         unsweptBefore, unsweptAfter,
         totalRetained, activeBytes, emptySwept);
}

void CentralRegionPool::VisitMarkPendingRegionsUnderSTW(
    const std::function<void(MSRegionInfo*)>& visitor)
{
    constexpr uint8_t kHasRef = ToMSScanClassIndex(MSScanClass::HAS_REF);
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        collectSet_[cls][kHasRef].ForEach(visitor);
        buckets_[cls][kHasRef].VisitAllUnsafe(visitor);
    }

    VisitAllocBufferMS([&visitor](AllocBufferMS& buffer) {
        for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
            MSRegionInfo* region =
                buffer.GetActiveRegion(cls, MSScanClass::HAS_REF);
            if (region != nullptr) {
                visitor(region);
            }
        }
    });
}

void CentralRegionPool::ReenableSweepAdmission()
{
    skipAssistSweep_.store(false, std::memory_order_release);
    skipHelperSweep_.store(false, std::memory_order_release);
    stopSweepRequested_.store(false, std::memory_order_release);
    OpenSweepAdmission();
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
