// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_HEAP_ACCOUNTING_H
#define MRT_MS_HEAP_ACCOUNTING_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>

#include "Heap/MCollector/MSMemoryLimitPolicy.h"

namespace MapleRuntime {

class CentralRegionPool;
class GCStats;
class RegionSpaceMS;

// Runtime policy and allocator accounting remain in bytes. REPORT logging uses
// this conversion at its boundary so every MSGC capacity field shares MB.
inline double MSBytesToMB(size_t bytes)
{
    constexpr double kBytesPerMB = 1024.0 * 1024.0;
    return static_cast<double>(bytes) / kBytesPerMB;
}

// A best-effort physical-heap sample. Its counters are independently published
// by the allocator, so it is deliberately a point-in-time policy input rather
// than a globally cached, transactionally consistent heap image.
struct MSHeapRetentionSnapshot {
    size_t activeBytes = 0;
    size_t emptyCacheBytes = 0;
    size_t hotFreeBytes = 0;

    size_t RetainedBytes() const;
    size_t ReclaimableBytes() const;
};

// A current Sweep-epoch sample. This is kept separate from retention because
// Sweep scheduling needs progress and epoch identity rather than cache state.
struct MSSweepProgressSnapshot {
    size_t sweptBytes = 0;
    size_t unsweptRegions = 0;
    size_t unsweptBytes = 0;
    uint32_t epochId = 0;
};

// A best-effort Region topology sample for REPORT logging.  It deliberately
// reuses the allocator's published aggregates instead of making every GC log
// site walk per-size-class lists or FreeManager shards independently.
struct MSGCRegionSummary {
    size_t processorCount = 0;
    size_t allocBufferActiveRegions = 0;
    size_t centralSizeClasses = 0;
    size_t centralNonEmptySizeClasses = 0;
    size_t centralSmallRegions = 0;
    size_t sweptRegions = 0;
    size_t unsweptRegions = 0;
    size_t emptyRegions = 0;
    size_t largeObjects = 0;
    size_t largeSpanRegions = 0;
    size_t hotFreeRegions = 0;
    size_t committedRegions = 0;
    size_t committedBytes = 0;
};

// Best-effort values published at the end of the preceding GC by
// UpdateGCStats(). They are independently atomic rather than a transactional
// image, and remain policy inputs for the next allocation/Scavenge interval.
struct MSHeapPolicySnapshot {
    size_t markThresholdBytes = 0;
    size_t lastMarkOutsideBytes = 0;
    size_t scavengeLowBytes = 0;
    size_t scavengeHighBytes = 0;
    size_t softLimitBytes = 0;
};

// One mark-trigger evaluation and the exact values that produced it. Keeping
// them together prevents the allocation path from recomputing a different
// effective-swept value solely for diagnostics.
struct MSMarkTriggerDecision {
    MSMarkTrigger trigger = MSMarkTrigger::NONE;
    size_t retainedBytes = 0;
    size_t sweptBytes = 0;
    size_t outsideMarkBytes = 0;
    size_t effectiveSweptBytes = 0;
    size_t thresholdBytes = 0;
    size_t softLimitBytes = 0;
};

// Capture the O(1) aggregate counters used on allocation and background-work
// paths. The returned value can be reused by one caller without re-sampling.
MSHeapRetentionSnapshot CaptureMSHeapRetentionSnapshot(const RegionSpaceMS& space);

// Capture retention for detailed diagnostics. Only hot-free bytes use the
// per-shard FreeManager scan; active and empty-cache bytes already come from
// their O(1) central-pool aggregates.
MSHeapRetentionSnapshot CaptureMSHeapRetentionDiagnosticSnapshot(const RegionSpaceMS& space);

// Capture Sweep progress from CentralRegionPool's published aggregate counters.
MSSweepProgressSnapshot CaptureMSSweepProgressSnapshot(const CentralRegionPool& pool);

// Capture the stable, compact Region summary shared by GC-begin and GC-end
// REPORT logs. This is diagnostic-only and may race mutator allocation outside
// STW; each field remains an independently published point-in-time value.
MSGCRegionSummary CaptureMSGCRegionSummary(const RegionSpaceMS& space);

// Load the policy values published by the previous UpdateGCStats() call,
// including the runtime's current configured soft heap limit.
MSHeapPolicySnapshot LoadMSHeapPolicySnapshot(const GCStats& gcStats);

// Pure decisions over caller-owned snapshots.
size_t ComputeMSScavengeReleaseTarget(const MSHeapRetentionSnapshot& retention,
                                      const MSHeapPolicySnapshot& policy);
MSMarkTriggerDecision EvaluateMSMarkTrigger(const MSHeapRetentionSnapshot& retention,
                                            const MSSweepProgressSnapshot& sweep,
                                            const MSHeapPolicySnapshot& policy);

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_HEAP_ACCOUNTING_H
