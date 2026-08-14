// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/MCollector/MSHeapAccounting.h"

#ifdef MRT_USE_MARK_SWEEP

#include <algorithm>
#include <limits>

#include "CangjieRuntime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSRegionManager.h"
#include "Heap/MAllocator/MSRegionSpace.h"

namespace MapleRuntime {

namespace {

size_t SaturatingAddMSAccountingBytes(size_t lhs, size_t rhs)
{
    constexpr size_t max = std::numeric_limits<size_t>::max();
    return rhs > max - lhs ? max : lhs + rhs;
}

size_t MSRegionsToBytes(size_t regionCount)
{
    constexpr size_t max = std::numeric_limits<size_t>::max();
    return regionCount > max / MS_REGION_SIZE ? max : regionCount * MS_REGION_SIZE;
}

} // namespace

size_t MSHeapRetentionSnapshot::RetainedBytes() const
{
    return SaturatingAddMSAccountingBytes(
        SaturatingAddMSAccountingBytes(activeBytes, emptyCacheBytes), hotFreeBytes);
}

size_t MSHeapRetentionSnapshot::ReclaimableBytes() const
{
    return SaturatingAddMSAccountingBytes(emptyCacheBytes, hotFreeBytes);
}

namespace {

MSHeapRetentionSnapshot CaptureMSHeapRetentionSnapshotImpl(
    const RegionSpaceMS& space, bool referenceHotFree)
{
    MSHeapRetentionSnapshot snapshot;
    const CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        snapshot.activeBytes = MSRegionsToBytes(centralPool->GetActiveMSRegionCount());
        snapshot.emptyCacheBytes = MSRegionsToBytes(centralPool->GetEmptySweptRegionCount());
    }

    const MSRegionManager* regionManager = space.GetMSRegionManager();
    if (regionManager != nullptr) {
        snapshot.hotFreeBytes = referenceHotFree
            ? regionManager->GetHotFreeBytesReference()
            : regionManager->GetHotFreeBytes();
    }
    return snapshot;
}

} // namespace

MSHeapRetentionSnapshot CaptureMSHeapRetentionSnapshot(const RegionSpaceMS& space)
{
    return CaptureMSHeapRetentionSnapshotImpl(space, false);
}

MSHeapRetentionSnapshot CaptureMSHeapRetentionDiagnosticSnapshot(const RegionSpaceMS& space)
{
    return CaptureMSHeapRetentionSnapshotImpl(space, true);
}

MSSweepProgressSnapshot CaptureMSSweepProgressSnapshot(const CentralRegionPool& pool)
{
    MSSweepProgressSnapshot snapshot;
    snapshot.sweptBytes = MSRegionsToBytes(pool.GetSweptMSRegionCount());
    snapshot.unsweptRegions = pool.GetTotalUnsweptCount();
    snapshot.unsweptBytes = MSRegionsToBytes(snapshot.unsweptRegions);
    snapshot.epochId = pool.GetSweepCycleId();
    return snapshot;
}

MSGCRegionSummary CaptureMSGCRegionSummary(const RegionSpaceMS& space)
{
    MSGCRegionSummary summary;
    if (Runtime::CurrentRef() != nullptr) {
        summary.processorCount =
            Runtime::Current().GetConcurrencyModel().GetProcessorNum();
    }

    const CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool != nullptr) {
        summary.allocBufferActiveRegions =
            centralPool->GetAllAllocBufferActiveRegionCount();
        summary.centralSizeClasses = kNumSizeClasses;
        for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
            CentralRegionPool::BucketPressure pressure =
                centralPool->GetBucketPressure(cls);
            size_t regions = SaturatingAddMSAccountingBytes(
                SaturatingAddMSAccountingBytes(
                    pressure.partialSwept, pressure.fullSwept),
                pressure.unswept);
            if (regions != 0) {
                ++summary.centralNonEmptySizeClasses;
            }
        }
        summary.centralSmallRegions = SaturatingAddMSAccountingBytes(
            centralPool->GetSmallSweptRegionCount(),
            centralPool->GetTotalUnsweptCount());
        summary.sweptRegions = centralPool->GetSweptMSRegionCount();
        summary.unsweptRegions = centralPool->GetTotalUnsweptCount();
        summary.emptyRegions = centralPool->GetEmptySweptRegionCount();
        summary.largeObjects = centralPool->GetLargeRegionCount();
        summary.largeSpanRegions = centralPool->GetLargeLiveSpanCount();
    }

    const MSRegionManager* regionManager = space.GetMSRegionManager();
    if (regionManager != nullptr) {
        summary.hotFreeRegions =
            regionManager->GetHotFreeBytes() / MS_REGION_SIZE;
        summary.committedRegions = regionManager->GetMaxRegionCount();
        summary.committedBytes = regionManager->GetCurrentCapacityBytes();
    }
    return summary;
}

MSHeapPolicySnapshot LoadMSHeapPolicySnapshot(const GCStats& gcStats)
{
    MSHeapPolicySnapshot snapshot;
    snapshot.markThresholdBytes = gcStats.GetThreshold();
    snapshot.lastMarkOutsideBytes = gcStats.GetLastMarkOutsideBytes();
    snapshot.scavengeLowBytes = gcStats.GetScavengeThreshold();
    snapshot.scavengeHighBytes = gcStats.GetScavengeTriggerThreshold();
    snapshot.softLimitBytes = CangjieRuntime::GetMSSoftHeapLimitBytes();
    return snapshot;
}

size_t ComputeMSScavengeReleaseTarget(const MSHeapRetentionSnapshot& retention,
                                      const MSHeapPolicySnapshot& policy)
{
    if (policy.scavengeLowBytes == 0 || policy.scavengeHighBytes == 0) {
        return 0;
    }
    size_t effectiveLowTarget = std::max(policy.scavengeLowBytes, retention.activeBytes);
    return ComputeMSScavengeWatermarkReleaseGap(
        retention.RetainedBytes(), retention.ReclaimableBytes(),
        effectiveLowTarget, policy.scavengeHighBytes);
}

MSMarkTriggerDecision EvaluateMSMarkTrigger(const MSHeapRetentionSnapshot& retention,
                                            const MSSweepProgressSnapshot& sweep,
                                            const MSHeapPolicySnapshot& policy)
{
    MSMarkTriggerDecision decision;
    decision.retainedBytes = retention.RetainedBytes();
    decision.sweptBytes = sweep.sweptBytes;
    decision.outsideMarkBytes = policy.lastMarkOutsideBytes;
    decision.effectiveSweptBytes = SaturatingSubtractMSBytes(
        decision.sweptBytes, decision.outsideMarkBytes);
    decision.thresholdBytes = policy.markThresholdBytes;
    decision.softLimitBytes = policy.softLimitBytes;
    decision.trigger = ResolveMSMarkTrigger(
        decision.retainedBytes, decision.effectiveSweptBytes,
        decision.thresholdBytes, decision.softLimitBytes);
    return decision;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
