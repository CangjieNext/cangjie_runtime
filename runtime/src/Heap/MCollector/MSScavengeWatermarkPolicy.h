// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_SCAVENGE_WATERMARK_POLICY_H
#define MRT_MS_SCAVENGE_WATERMARK_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "Heap/MCollector/MSHeapThresholdPolicy.h"

namespace MapleRuntime {

// Scavenge uses a low/high retained-heap band instead of releasing every
// Region immediately after it becomes reclaimable. The low watermark keeps
// the capacity used in the just-finished allocation period or the current
// HeapGrow heap goal, whichever is larger. The high watermark is only a
// full allocation runway that decides when background Scavenge starts.
struct MSScavengeWatermarkDecision {
    size_t retainLowBytes = 0;
    size_t retainHighBytes = 0;
};

constexpr uint64_t kMSScavengeHighDecayIntervalUs = 10000000ULL;

// Low follows the most recent pre-sweep footprint directly. High is a cache
// admission gate and must not fall on every short GC-cycle fluctuation: it
// rises immediately, then gives back no more than one Segment per 10 seconds.
struct MSScavengeHighWatermarkDecision {
    size_t retainHighBytes = 0;
    uint64_t decayIntervals = 0;
};

inline MSScavengeWatermarkDecision EvaluateMSScavengeWatermarks(
    size_t heapGoalBytes, size_t preSweepInUseBytes,
    size_t softLimitBytes, size_t segmentBytes)
{
    MSScavengeWatermarkDecision decision;
    size_t minimumBand = std::max<size_t>(segmentBytes, 1);
    // retainLow is the footprint required by the allocation period that just
    // completed. A transient in-use spike must not also reserve a second full
    // heap goal above it. Keep one Segment above low and a stable 1.5-goal
    // cache floor, which matches the observed steady Region footprint.
    if (softLimitBytes == 0) {
        decision.retainLowBytes = std::max(heapGoalBytes, preSweepInUseBytes);
        decision.retainHighBytes = std::max(
            SaturatingAddMSBytes(decision.retainLowBytes, minimumBand),
            SaturatingMultiplyMSBytes(heapGoalBytes, 3) / 2);
        return decision;
    }

    size_t emergencyReserve = std::max(
        SaturatingMultiplyMSBytes(segmentBytes, 2), softLimitBytes / 16);
    size_t retainBudgetBytes = emergencyReserve >= softLimitBytes
        ? 0
        : softLimitBytes - emergencyReserve;
    size_t boundedHeapGoal = std::min(heapGoalBytes, retainBudgetBytes);
    decision.retainLowBytes = std::max(preSweepInUseBytes, boundedHeapGoal);
    size_t stableCacheFloor = SaturatingMultiplyMSBytes(boundedHeapGoal, 3) / 2;
    size_t lowWatermarkRunway = SaturatingAddMSBytes(
        decision.retainLowBytes, minimumBand);
    size_t desiredHighBytes = std::max(lowWatermarkRunway, stableCacheFloor);
    decision.retainHighBytes = std::max(
        decision.retainLowBytes,
        std::min(retainBudgetBytes, desiredHighBytes));
    return decision;
}

inline MSScavengeHighWatermarkDecision StabilizeMSScavengeHighWatermark(
    size_t desiredHighBytes, size_t retainLowBytes,
    size_t previousHighBytes, uint64_t elapsedDecayUs, size_t segmentBytes,
    bool forceDecrease = false)
{
    MSScavengeHighWatermarkDecision decision;
    size_t boundedDesiredHighBytes = std::max(desiredHighBytes, retainLowBytes);
    if (previousHighBytes == 0 || boundedDesiredHighBytes >= previousHighBytes) {
        decision.retainHighBytes = boundedDesiredHighBytes;
        return decision;
    }

    if (forceDecrease) {
        decision.retainHighBytes = boundedDesiredHighBytes;
        return decision;
    }

    decision.decayIntervals = elapsedDecayUs / kMSScavengeHighDecayIntervalUs;
    if (decision.decayIntervals == 0) {
        decision.retainHighBytes = previousHighBytes;
        return decision;
    }

    size_t decayStepCount = decision.decayIntervals >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max())
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(decision.decayIntervals);
    size_t decayBytes = SaturatingMultiplyMSBytes(
        std::max<size_t>(segmentBytes, 1), decayStepCount);
    size_t decayedHighBytes = previousHighBytes > decayBytes
        ? previousHighBytes - decayBytes
        : 0;
    decision.retainHighBytes = std::max(
        retainLowBytes,
        std::max(boundedDesiredHighBytes, decayedHighBytes));
    return decision;
}

inline size_t ComputeMSScavengeThreshold(
    size_t gcGoalBytes, size_t maxHeapBytes, double scavengeGrowth)
{
    long double boundedGrowth = std::max(
        static_cast<long double>(scavengeGrowth), 1.0L);
    size_t thresholdBytes = ScaleMSHeapThreshold(gcGoalBytes, boundedGrowth);
    if (maxHeapBytes != 0) {
        thresholdBytes = std::min(thresholdBytes, maxHeapBytes);
    }
    return thresholdBytes;
}

// Static MSGC mode preserves the user-facing cjScavengeGrowth contract. The
// configured multiplier controls the high wake-up watermark directly, while
// the low target remains the just-observed in-use footprint. The RegionSpace
// release path still clamps low to its current in-use bytes before releasing.
inline MSScavengeWatermarkDecision EvaluateMSStaticScavengeWatermarks(
    size_t heapGoalBytes, size_t preSweepInUseBytes,
    size_t softLimitBytes, double scavengeGrowth)
{
    MSScavengeWatermarkDecision decision;
    size_t retainBudgetBytes = 0;
    if (softLimitBytes == 0) {
        retainBudgetBytes = std::numeric_limits<size_t>::max();
    } else {
        size_t emergencyReserve = softLimitBytes / 16;
        retainBudgetBytes = emergencyReserve >= softLimitBytes
            ? 0
            : softLimitBytes - emergencyReserve;
    }
    decision.retainLowBytes = std::min(preSweepInUseBytes,
                                       retainBudgetBytes);
    size_t staticThreshold = ComputeMSScavengeThreshold(
        heapGoalBytes, retainBudgetBytes, scavengeGrowth);
    decision.retainHighBytes = std::max(decision.retainLowBytes,
                                        staticThreshold);
    return decision;
}

} // namespace MapleRuntime

#endif // MRT_MS_SCAVENGE_WATERMARK_POLICY_H
