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
#include "Heap/MCollector/MSHeapAccounting.h"
#include "Heap/MCollector/MSGCWorkerPoolSession.h"
#include "Common/BaseObject.h"
#include "Base/CString.h"
#include "Base/EnvParam.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/SysCall.h"
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
constexpr size_t kEmptyDrainBatchSize = 64;

bool IsSTWSweepDiagnosticEnabledImpl()
{
    static const bool enabled = ReadBoolParameter("CJ_MS_STW_SWEEP", false);
    return enabled;
}

void ClearSmallRegionPayload(uintptr_t regionStart)
{
    memset(reinterpret_cast<void*>(regionStart), 0, MS_REGION_SIZE);
}

}

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

bool CentralRegionPool::IsSTWSweepDiagnosticEnabled()
{
    return IsSTWSweepDiagnosticEnabledImpl();
}

void CentralRegionPool::PublishSweepDoneAfterQuiescence()
{
    CHECK(!sweepEpoch_.IsForceFinishRunning());
    CHECK(!bgSweepRunning_.load(std::memory_order_acquire));
    CHECK(GetTotalUnsweptCount() == 0);
    CHECK(GetTotalUnsweptCountReference() == 0);

    // Publish completion first. A concurrent ForceHighSweepPressure() either
    // observes the coordinator's completed epoch and returns, or rechecks it after setting the latch
    // and clears its own stale HIGH. Clearing the latch before this store would
    // allow the notifier to leave forced HIGH behind on an already-done epoch.
    sweepEpoch_.PublishSweepDoneAfterQuiescence();
    sweepCycleState_.forceHighPressure.store(false, std::memory_order_release);
    sweepCycleState_.pressureMode.store(SweepPressureMode::LOW,
                                        std::memory_order_release);
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
    if (region->IsMutatorReturnedSwept()) {
        region->ClearMutatorReturnedSwept();
        size_t previous = centralSmallMutatorReturnedSweptRegions_.fetch_sub(
            1, std::memory_order_acq_rel);
        CHECK_DETAIL(previous != 0,
                     "[MS-GC] mutator-returned swept Region count underflow");
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

void CentralRegionPool::ReturnMutatorPartialRegion(MSRegionInfo* region, uint8_t cls)
{
    CHECK_DETAIL(region != nullptr, "ReturnMutatorPartialRegion with null region");
    CHECK_DETAIL(!region->IsMutatorReturnedSwept(),
                 "ReturnMutatorPartialRegion with already-accounted region");
    // Publish provenance and its aggregate before the Region becomes visible
    // on the lock-free central list.
    region->MarkMutatorReturnedSwept();
    centralSmallMutatorReturnedSweptRegions_.fetch_add(1, std::memory_order_release);
    ReturnPartialRegion(region, cls);
}

void CentralRegionPool::ReturnMutatorFullRegion(MSRegionInfo* region, uint8_t cls)
{
    CHECK_DETAIL(region != nullptr, "ReturnMutatorFullRegion with null region");
    CHECK_DETAIL(!region->IsMutatorReturnedSwept(),
                 "ReturnMutatorFullRegion with already-accounted region");
    region->MarkMutatorReturnedSwept();
    centralSmallMutatorReturnedSweptRegions_.fetch_add(1, std::memory_order_release);
    ReturnFullRegion(region, cls);
}

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

size_t CentralRegionPool::DrainEmptySweptBatch(size_t maxRegions, std::vector<MSRegionInfo*>& drained)
{
    drained.clear();
    if (maxRegions == 0) {
        return 0;
    }

    size_t batchLimit = std::min(maxRegions, kEmptyDrainBatchSize);
    {
        std::lock_guard<std::mutex> lg(emptySweptLock_);
        for (size_t i = 0; i < batchLimit; ++i) {
            MSRegionInfo* region = emptySweptCache_.TakeHead();
            if (region == nullptr) {
                break;
            }
            drained.push_back(region);
        }
    }

    size_t drainedCount = drained.size();
    DecrementEmptySweptCount(drainedCount);
    return drainedCount;
}

size_t CentralRegionPool::ReclaimEmptyRegion(size_t maxRegions)
{
    size_t emptyCount = GetEmptySweptRegionCount();
    if (emptyCount == 0 || maxRegions == 0) {
        return 0;
    }

    size_t remainingRegions = std::min(emptyCount, maxRegions);
    std::vector<MSRegionInfo*> reclaimed;
    reclaimed.reserve(std::min(remainingRegions, kEmptyDrainBatchSize));
    size_t promotedBytes = 0;
    std::vector<FreeManager::RegionSpan> spans;
    spans.reserve(std::min(remainingRegions, kEmptyDrainBatchSize));
    while (remainingRegions != 0) {
        size_t reclaimedCount = DrainEmptySweptBatch(remainingRegions, reclaimed);
        if (reclaimedCount == 0) {
            break;
        }

        spans.clear();
        for (MSRegionInfo* region : reclaimed) {
            size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
            CHECK_DETAIL(region->IsFreeRegion(),
                         "[MS-GC] ReclaimEmptyRegion: bad regionType=%u idx=%zu",
                         static_cast<unsigned>(region->GetRegionType()), regionIdx);
            region->Reset();
            spans.push_back({ static_cast<FreeManager::RegionIndex>(regionIdx), 1 });
        }
        msRegionManager_->ReclaimReleasedRegionsToHotFreeBatch(spans);
        promotedBytes += reclaimedCount * MS_REGION_SIZE;
        remainingRegions -= reclaimedCount;
    }

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

uint32_t CentralRegionPool::PrepareMarkEpochUnderSTW(bool forceMaxHelpers)
{
    uint64_t prepareStartUs = TimeUtil::MicroSeconds();
    uint32_t epoch = currentMarkEpoch_.load(std::memory_order_relaxed) + 1;
    if (UNLIKELY(epoch == MSRegionInfo::NO_COLLECT_EPOCH)) {
        epoch = 1;
    }

    // Step 1: validate and freeze the previous-cycle collection boundary.
    // Purpose: no Region may be carried in collectSet_ or largeCollectSet_
    // before we move this cycle's swept supply into its immutable boundary.
    size_t staleSmallCollectSet = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            staleSmallCollectSet += collectSet_[cls][scan].count;
        }
    }
    size_t staleLargeCollectSet = 0;
    {
        std::lock_guard<std::mutex> lg(largeLock_);
        staleLargeCollectSet = largeCollectSet_.size();
    }
    CHECK_DETAIL(
        staleSmallCollectSet == 0 && staleLargeCollectSet == 0,
        "[MS-GC] PrepareMarkEpoch: stale collection boundary small=%zu large=%zu",
        staleSmallCollectSet, staleLargeCollectSet);

    size_t estimatedResetBytes = 0;
    struct ResetLane {
        RegionChain* chain;
    };
    std::array<ResetLane, kNumSizeClasses * kMSScanClassCount> resetLanes{};
    size_t resetLaneCount = 0;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            RegionChain& chain = collectSet_[cls][scan];
            buckets_[cls][scan].StealSweptToCollectSet(chain);
            if (!chain.IsEmpty()) {
                resetLanes[resetLaneCount++] = {&chain};
                size_t bitmapBytes = static_cast<size_t>(SizeClassInfo::ClassToBitmapWords(cls)) *
                    sizeof(uint64_t);
                estimatedResetBytes += chain.count * bitmapBytes *
                    (scan == ToMSScanClassIndex(MSScanClass::HAS_REF) ? 2 : 1);
            }
        }
    }

    // Step 2: reset mark workspace for the frozen Small collect-set.
    // Purpose: each sealed Region enters the new epoch with empty MARK/SCAN
    // planes. Independent size-class lanes can use the GC worker pool here.
    auto resetSmallLane = [epoch](RegionChain& chain) {
        chain.ForEach([epoch](MSRegionInfo* region) {
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
    bool parallelResetStarted = false;
    MSGCWorkerPoolSession resetSession(
        resources, MSGCWorkerPoolOwner::PRE_ENUM, false);

    if (helperCount != 0 && resetSession.Acquire()) {
        std::array<std::unique_ptr<HeapWork>, kNumSizeClasses * kMSScanClassCount> stagedTasks{};
        bool allocationFailed = false;
        for (size_t laneIndex = 0; laneIndex < resetLaneCount; ++laneIndex) {
            const ResetLane lane = resetLanes[laneIndex];
            std::unique_ptr<HeapWork> task(new (std::nothrow) LambdaWork(
                [chain = lane.chain, resetSmallLane](size_t) {
                    resetSmallLane(*chain);
                }));
            if (task == nullptr) {
                allocationFailed = true;
                break;
            }
            stagedTasks[laneIndex] = std::move(task);
        }
        if (allocationFailed) {
            resetSession.Finish();
        } else {
            for (size_t laneIndex = 0; laneIndex < resetLaneCount; ++laneIndex) {
                resetSession.AddWork(stagedTasks[laneIndex].release());
            }
            resetSession.Start(helperCount);
            parallelResetStarted = true;
        }
    }

    if (!parallelResetStarted) {
        for (size_t laneIndex = 0; laneIndex < resetLaneCount; ++laneIndex) {
            const ResetLane lane = resetLanes[laneIndex];
            resetSmallLane(*lane.chain);
        }
    }

    // Step 3: freeze and reset LARGE membership.
    // Purpose: the O(1) swap makes the current LARGE set immutable for Mark;
    // later LARGE allocations append only to largeNextSupply_ for next cycle.
    {
        std::lock_guard<std::mutex> lg(largeLock_);
        largeCollectSet_.swap(largeNextSupply_);
        for (uint32_t idx : largeCollectSet_) {
            MSRegionInfo* region =
                MSRegionInfoArray::GetRegionInfoByIdx(idx);
            CHECK_DETAIL(
                region != nullptr &&
                region->GetRegionKind() == MSRegionKind::LARGE &&
                region->GetMarkTaskNextRegionIdx() == MSRegionInfo::INVALID_REGION_IDX,
                "[MS-GC] PrepareMarkEpoch: invalid LARGE entry idx=%u",
                idx);
            region->ClearMarkedFlag();
            region->collectEpoch = epoch;
        }
    }

    // Step 4: discard active-Region producer residue without collecting it.
    // Purpose: TLS active Regions remain mutator-owned and outside this
    // boundary. Clear stale barrier/assist state plus MARK/SCAN workspace only;
    // never materialize or inspect their live ALLOC plane.
    VisitAllocBufferMS([](AllocBufferMS& buffer) {
        ResetMSBarrierMarkBuffer(buffer.GetBarrierMarkBuffer());
        buffer.ResetGCMarkAssistCycleState();
        for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
            for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
                MSRegionInfo* region =
                    buffer.GetActiveRegion(cls, static_cast<MSScanClass>(scan));
                if (region == nullptr) {
                    continue;
                }
                region->collectEpoch = MSRegionInfo::NO_COLLECT_EPOCH;
                region->ResetSmallMarkWorkState();
            }
        }
    });

    if (parallelResetStarted) {
        resetSession.Finish();
    }

    // Every swept small Region has now moved into this cycle's immutable
    // collect-set. Reset the corresponding central-supply provenance before
    // mutators resume; regions Sweep later returns are deliberately untagged.
    centralSmallMutatorReturnedSweptRegions_.store(0, std::memory_order_release);

    // Step 5: publish the completed epoch after every reset worker has exited.
    // Purpose: the release stores form the single visibility boundary: a
    // mutator can observe this epoch only after all Small, LARGE, and active
    // Region state plus barrier producer residue have been reset under STW.
    currentMarkEpoch_.store(epoch, std::memory_order_release);
    PublishMSGCFastPathMarkEpoch(epoch);

    VLOG(REPORT, "[MS-GC]   PrepareMarkEpoch STW: total=%lluus",
         static_cast<unsigned long long>(
             TimeUtil::MicroSeconds() - prepareStartUs));
    return epoch;
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

    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    MSHeapPolicySnapshot policy = LoadMSHeapPolicySnapshot(
        Heap::GetHeap().GetCollector().GetGCStats());
    // The epoch's base assist ratio is paced against actual allocation
    // ownership, not sweep-list membership. Sweep progress remains the input
    // to executor pressure decisions after this epoch is opened.
    MSMarkTriggerSnapshot markTrigger = CaptureMSMarkTriggerSnapshot(*this);
    size_t runwayRegions = CeilDivideMSSweepValue(
        ComputeMSSweepRunwayBytes(
            policy.markThresholdBytes, markTrigger.triggerBytes),
        MS_REGION_SIZE);
    bool stwSweepDiagnostic = IsSTWSweepDiagnosticEnabled();
    bool disableAssist = CangjieRuntime::IsMSAssistDisabled();
    uint64_t sweepAssistRatio = disableAssist || stwSweepDiagnostic
        ? 0
        : ComputeMSSweepAssistRatioScaled(totalUnswept, runwayRegions);
    sweepCycleState_.ResetForNewEpoch(sweepAssistRatio);
    sweepEpochInitialUnsweptRegions_.store(totalUnswept, std::memory_order_release);
    sweepEpoch_.BeginEpoch(totalUnswept != 0);
    uint32_t newSweepCycleId = sweepEpoch_.GetEpochId();
    MSRegionManager* regionManager = space.GetMSRegionManager();
    if (regionManager != nullptr) {
        regionManager->AdvanceScavengeEpoch(newSweepCycleId);
    }

    // Publish the complete Sweep epoch before any background, helper, or
    // mutator-assist claimant may enter it. The diagnostic mode keeps the
    // admission gate closed so PreEnumRoots() owns the sole serial drain.
    if (stwSweepDiagnostic) {
        stopSweepRequested_.store(true, std::memory_order_release);
        skipAssistSweep_.store(true, std::memory_order_release);
        skipHelperSweep_.store(true, std::memory_order_release);
    } else {
        ReenableSweepAdmission();
    }

    VLOG(REPORT,
         "[MS-GC]   Publish sweep: epoch=%u unsweptRegions=%zu assistRatio=%llu",
         static_cast<unsigned>(newSweepCycleId),
         totalUnswept,
         static_cast<unsigned long long>(sweepAssistRatio));
}

// ============================================================================
// BackgroundSweep — called by gc-helper thread (BackgroundProcessor)
// ============================================================================

void CentralRegionPool::BackgroundSweep(uint8_t preferredCls, uint64_t deadlineUs)
{
    if (!sweepEpoch_.TryEnterSweep()) {
        return;
    }
    bgSweepRunning_.store(true, std::memory_order_release);
    auto finish = [this]() {
        bgSweepRunning_.store(false, std::memory_order_release);
        sweepEpoch_.LeaveSweep();
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

    auto classOrder = BuildBackgroundSweepClassOrder(preferredCls);
    (void)SweepClassOrder(classOrder, sweptInSlice, deadlineUs);

    RecordNonAssistSweep(sweptInSlice);
    finish();
}

void CentralRegionPool::ParallelSweep(uint8_t preferredCls, uint64_t deadlineUs)
{
    if (!sweepEpoch_.TryEnterSweep()) {
        return;
    }
    if (skipHelperSweep_.load(std::memory_order_acquire) ||
        stopSweepRequested_.load(std::memory_order_acquire)) {
        sweepEpoch_.LeaveSweep();
        return;
    }

    size_t sweptInSlice = 0;
    auto classOrder = BuildMSSweepCyclicClassOrder<kNumSizeClasses>(preferredCls);
    (void)SweepClassOrder(classOrder, sweptInSlice, deadlineUs);

    RecordNonAssistSweep(sweptInSlice);
    sweepEpoch_.LeaveSweep();
}

MSSweepAssistResult CentralRegionPool::MSSweepAssist(size_t debtRegions, uint8_t preferredCls)
{
    MSSweepAssistResult result;
    if (debtRegions == 0 || sweepEpoch_.IsSweepDone() ||
        GetTotalUnsweptCount() == 0) {
        result.sweepComplete = sweepEpoch_.IsSweepDone();
        return result;
    }

    // Background/lazy/helper work is a one-shot global credit. CAS claiming is
    // the ownership transfer: once one P consumes it, no other P can use the
    // same swept Region to reduce its local debt.
    result.creditPaidRegions = sweepCycleState_.credit.Take(debtRegions);
    size_t remainingDebt = debtRegions - result.creditPaidRegions;
    if (remainingDebt == 0) {
        result.sweepComplete = sweepEpoch_.IsSweepDone();
        return result;
    }

    // Pressure scales debt addition, never repayment speed. Retain a sub-batch
    // remainder; once it reaches the configured threshold, run one exact batch.
    size_t toSweep = ComputeMSSweepAssistDirectRegions(
        remainingDebt, CangjieRuntime::GetGCParam().msAssistSweepMaxRegionsPerCall);
    if (toSweep == 0) {
        result.sweepComplete = sweepEpoch_.IsSweepDone();
        return result;
    }

    // AssistSweep carries the unified admission guard. GC closes that guard
    // before clearing mark bitmaps, so no new SweepRegion can start afterward.
    result.assistPaidRegions = AssistSweep(toSweep, preferredCls);
    result.sweepComplete = sweepEpoch_.IsSweepDone();
    return result;
}

SweepPressureMode CentralRegionPool::RefreshSweepDebtSnapshot(size_t thresholdBytes, size_t sweptBytes)
{
    size_t unsweptBytes = GetTotalUnsweptBytes();
    bool forcedHigh = sweepCycleState_.forceHighPressure.load(
        std::memory_order_acquire);
    SweepPressureMode previousMode = GetSweepPressureMode();
    SweepPressureMode mode = sweepEpoch_.IsSweepDone() || unsweptBytes == 0
        ? SweepPressureMode::LOW
        : EvaluateSweepPressure(unsweptBytes, sweptBytes, thresholdBytes, forcedHigh, previousMode);
    if (unsweptBytes == 0) {
        sweepCycleState_.forceHighPressure.store(false,
                                                  std::memory_order_release);
    }
    sweepCycleState_.pressureMode.store(mode, std::memory_order_release);

    return mode;
}

void CentralRegionPool::ForceHighSweepPressure()
{
    if (sweepEpoch_.IsSweepDone() || GetTotalUnsweptCount() == 0) {
        return;
    }
    sweepCycleState_.forceHighPressure.store(true, std::memory_order_release);
    sweepCycleState_.pressureMode.store(SweepPressureMode::HIGH,
                                        std::memory_order_release);
    // Close the race where the final Sweep publisher completed immediately
    // before these stores. A completed epoch must never retain forced HIGH.
    if (sweepEpoch_.IsSweepDone()) {
        sweepCycleState_.forceHighPressure.store(false,
                                                  std::memory_order_release);
        sweepCycleState_.pressureMode.store(SweepPressureMode::LOW,
                                            std::memory_order_release);
    }
}

size_t CentralRegionPool::AssistSweep(size_t targetRegions, uint8_t preferredCls)
{
    if (targetRegions == 0) {
        return 0;
    }

    if (!sweepEpoch_.TryEnterSweep()) {
        return 0;
    }
    if (skipAssistSweep_.load(std::memory_order_acquire) ||
        stopSweepRequested_.load(std::memory_order_acquire)) {
        sweepEpoch_.LeaveSweep();
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
    sweepEpoch_.LeaveSweep();
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

size_t CentralRegionPool::GetMutatorReturnedSweptRegionCount() const
{
    return centralSmallMutatorReturnedSweptRegions_.load(std::memory_order_acquire);
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
    forceFinishedSweepRegions_ += sweptRegions;
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

size_t CentralRegionPool::GetAllocBufferCount() const
{
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
    return msAllocBuffers_.size();
}

size_t CentralRegionPool::GetSweptMSRegionCount() const
{
    return centralSmallSweptRegions_.load(std::memory_order_relaxed) +
           largeLiveSpanCount_.load(std::memory_order_relaxed);
}

size_t CentralRegionPool::GetActiveMSRegionCount() const
{
    return centralSmallActiveRegions_.load(std::memory_order_relaxed) +
           largeLiveSpanCount_.load(std::memory_order_relaxed);
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
    uint64_t startUs = TimeUtil::MicroSeconds();
    // Two-pass to avoid holding largeLock_ during ReclaimRegion.
    std::vector<MSRegionInfo*> deadRegions;

    CollectDeadLargeRegions(
        currentMarkEpoch_.load(std::memory_order_acquire), deadRegions);

    size_t reclaimedBytes = 0;
    size_t reclaimedSpans = 0;
    // Release dead large regions.
    for (MSRegionInfo* msInfo : deadRegions) {
        uintptr_t regionStart = msInfo->LargeObjectStart();
        size_t regionBytes = msInfo->LargeSpanBytes();
        reclaimedBytes += regionBytes;
        size_t spanCount = msInfo->LargeSpanCount();
        reclaimedSpans += spanCount;

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

    VLOG(REPORT,
         "[MS-GC]   Large sweep: releasedLargeObjects=%zu releasedSpans=%zu "
         "reclaimedBytes=%.2fMB total=%lluus",
         deadRegions.size(), reclaimedSpans, MSBytesToMB(reclaimedBytes),
         static_cast<unsigned long long>(TimeUtil::MicroSeconds() - startUs));
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

MSMarkedStats CentralRegionPool::ComputeMarkedStats() const
{
    uint64_t startUs = TimeUtil::MicroSeconds();
    MSMarkedStats result;
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        const size_t slotSize = SizeClassInfo::ClassToSize(cls);
        const size_t wordCount = SizeClassInfo::ClassToBitmapWords(cls);
        for (uint8_t scan = 0; scan < kMSScanClassCount; ++scan) {
            const RegionChain& chain = collectSet_[cls][scan];
            chain.ForEach([cls, scan, slotSize, wordCount, &result](MSRegionInfo* region) {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
                CHECK_DETAIL(
                    region->GetRegionKind() == MSRegionKind::SMALL &&
                    region->GetAllocMode() == MSAllocMode::BITMAP &&
                    region->sizeClass == cls &&
                    ToMSScanClassIndex(region->GetScanClass()) == scan,
                    "[MS-GC] ComputeMarkedStats: invalid collect-set lane member "
                    "idx=%zu cls=%u/%u scan=%u/%u kind=%u mode=%u",
                    MSRegionInfoArray::GetRegionIdx(region),
                    static_cast<unsigned>(region->sizeClass), static_cast<unsigned>(cls),
                    static_cast<unsigned>(ToMSScanClassIndex(region->GetScanClass())),
                    static_cast<unsigned>(scan), static_cast<unsigned>(region->GetRegionKind()),
                    static_cast<unsigned>(region->GetAllocMode()));
#endif
                const uint64_t* markBitmap = region->GetMarkBitmap();
                size_t markedSlots = 0;
                for (size_t word = 0; word < wordCount; ++word) {
                    markedSlots += __builtin_popcountll(markBitmap[word]);
                }
                result.AddSlots(markedSlots, slotSize);
            });
        }
    }

    VisitLargeCollectSetLocked([&result](MSRegionInfo* region) {
        if (region->IsMarked()) {
            result.AddLargeObject(region->LargeSpanBytes());
        }
    });

    const RegionSpaceMS& space =
        static_cast<const RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    MSHeapRetentionSnapshot retention = CaptureMSHeapRetentionSnapshot(space);
    size_t inUseBytes = retention.activeBytes;
    size_t retainedBytes = retention.RetainedBytes();
    double fragmentationRatio = inUseBytes == 0 || result.liveBytes >= inUseBytes ? 0.0 :
        (static_cast<double>(inUseBytes - result.liveBytes) * 100.0) /
        static_cast<double>(inUseBytes);
    double liveRatio = retainedBytes == 0 ? 0.0 :
        (static_cast<double>(result.liveBytes) * 100.0) /
        static_cast<double>(retainedBytes);
    VLOG(REPORT,
         "[MS-GC]   PostMarkStats: liveBytes=%.2fMB inUse=%.2fMB "
         "fragmentation=%.1f%% retainedNoTLS=%.2fMB liveRatio=%.1f%% total=%lluus",
         MSBytesToMB(result.liveBytes), MSBytesToMB(inUseBytes), fragmentationRatio,
         MSBytesToMB(retainedBytes), liveRatio,
         static_cast<unsigned long long>(TimeUtil::MicroSeconds() - startUs));
    return result;
}

// ============================================================================
// VisitMarkedObjects: walk markBitmaps and call func() on every marked object.
// ============================================================================
// Same validity window as the post-Mark statistics task: marking complete, no
// sweep started. Used by the mark-completeness verifier to inspect every live object.
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
    uint64_t startUs = TimeUtil::MicroSeconds();
    // The GC main thread participates as an uncounted Sweep claimant below.
    // Keep completion closed across both drains and the intervening quiescence
    // waits so another executor cannot publish done after GC main claims the
    // final Region but before it finishes SweepRegion().
    sweepEpoch_.SetForceFinishRunning(true);
    forceFinishedSweepRegions_ = 0;

    // 1. Join the still-open Sweep epoch as another claimant unless the STW
    // diagnostic gate deliberately left all work for this serial closure.
    // This pass does not publish completion: claimed Regions may still be
    // inside SweepRegion on another thread.
    if (!IsSTWSweepDiagnosticEnabled()) {
        ForceFinishAllUnswept();
    }

    // 2. Close all Sweep admission paths before establishing quiescence. The
    // unified admission word makes closing and active-count accounting one
    // atomic protocol; these flags remain as lane-specific fast exits.
    stopSweepRequested_.store(true, std::memory_order_release);
    skipAssistSweep_.store(true, std::memory_order_release);
    skipHelperSweep_.store(true, std::memory_order_release);

    // 3. Wait for every admitted sweep source. A worker that starts after this
    // close cannot increment the packed active count.
    sweepEpoch_.CloseSweepAdmissionAndWait();

    // 4. Drain anything left unclaimed while admission was being
    // closed. With every in-flight source quiescent, this is the final serial
    // closure before mark bitmap clearing.
    ForceFinishAllUnswept();
    CHECK(GetTotalUnsweptCount() == 0);
    CHECK(GetTotalUnsweptCountReference() == 0);
    sweepEpoch_.SetForceFinishRunning(false);
    PublishSweepDoneAfterQuiescence();
    CHECK(IsSweepDone());

    VLOG(REPORT, "[MS-GC]   Sweep force finish: sweptRegions=%zu total=%lluus",
         forceFinishedSweepRegions_,
         static_cast<unsigned long long>(TimeUtil::MicroSeconds() - startUs));
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
    sweepEpoch_.OpenSweepAdmission();
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
