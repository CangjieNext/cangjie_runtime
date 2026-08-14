// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_HEAP_THRESHOLD_POLICY_H
#define MRT_MS_HEAP_THRESHOLD_POLICY_H

#include <algorithm>
#include <cstddef>
#include <limits>

namespace MapleRuntime {

constexpr size_t kMSHeapThresholdDeadbandDivisor = 10;
constexpr size_t kMSHeapThresholdTransitionDivisor = 8;

struct MSHeapThresholdDecision {
    size_t rawTarget = 0;
    size_t deadbandLower = 0;
    size_t deadbandUpper = 0;
    size_t riseLimit = 0;
    size_t fallLimit = 0;
    size_t threshold = 0;
    bool deadbandHeld = false;
    bool riseLimited = false;
    bool fallLimited = false;
    bool transitionLimited = false;
    bool transitionBypassed = false;
};

// Shared byte arithmetic for heap-threshold and Scavenge-watermark policies.
inline size_t SaturatingAddMSBytes(size_t lhs, size_t rhs)
{
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        return std::numeric_limits<size_t>::max();
    }
    return lhs + rhs;
}

inline size_t SaturatingMultiplyMSBytes(size_t lhs, size_t rhs)
{
    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    if (lhs > std::numeric_limits<size_t>::max() / rhs) {
        return std::numeric_limits<size_t>::max();
    }
    return lhs * rhs;
}

inline size_t ScaleMSHeapThreshold(size_t value, long double multiplier)
{
    if (multiplier <= 0.0L || value == 0) {
        return 0;
    }
    long double scaled = static_cast<long double>(value) * multiplier;
    if (scaled >= static_cast<long double>(std::numeric_limits<size_t>::max())) {
        return std::numeric_limits<size_t>::max();
    }
    return static_cast<size_t>(scaled);
}

inline size_t CeilDivideMSHeapThreshold(size_t value, size_t divisor)
{
    return value / divisor + static_cast<size_t>(value % divisor != 0);
}

inline size_t ComputeMSHeapThresholdFloor(size_t absoluteFloorBytes,
                                         size_t maxThresholdBytes)
{
    return std::min(absoluteFloorBytes, maxThresholdBytes);
}

// The active mark trigger threshold is derived from the current bitmap-live
// base and the adaptive growth controller. A stable normal cycle uses an
// inclusive 10% deadband and a 12.5% bounded transition; bootstrap, OOM and
// pressure cycles can establish their target immediately.
inline MSHeapThresholdDecision EvaluateMSHeapThreshold(
    size_t oldThreshold, size_t liveBytes, double heapGrowth,
    size_t minThreshold, size_t maxThreshold,
    bool bypassTransition = false)
{
    MSHeapThresholdDecision decision;
    minThreshold = std::min(minThreshold, maxThreshold);
    decision.rawTarget = ScaleMSHeapThreshold(
        liveBytes, 1.0L + static_cast<long double>(heapGrowth));
    decision.rawTarget = std::max(
        minThreshold, std::min(decision.rawTarget, maxThreshold));
    bool oldThresholdOutsideBounds =
        oldThreshold < minThreshold || oldThreshold > maxThreshold;
    if (bypassTransition || oldThreshold == 0 || oldThresholdOutsideBounds) {
        decision.threshold = decision.rawTarget;
        decision.transitionBypassed = true;
        return decision;
    }

    size_t deadbandDecrease =
        CeilDivideMSHeapThreshold(oldThreshold, kMSHeapThresholdDeadbandDivisor);
    size_t deadbandIncrease = oldThreshold / kMSHeapThresholdDeadbandDivisor;
    decision.deadbandLower = std::max(
        oldThreshold - deadbandDecrease, minThreshold);
    decision.deadbandUpper = std::min(
        SaturatingAddMSBytes(oldThreshold, deadbandIncrease), maxThreshold);

    size_t transitionDecrease =
        CeilDivideMSHeapThreshold(oldThreshold, kMSHeapThresholdTransitionDivisor);
    size_t transitionIncrease = oldThreshold / kMSHeapThresholdTransitionDivisor;
    decision.fallLimit = std::max(
        oldThreshold - transitionDecrease, minThreshold);
    decision.riseLimit = std::min(
        SaturatingAddMSBytes(oldThreshold, transitionIncrease), maxThreshold);

    if (decision.rawTarget >= decision.deadbandLower &&
        decision.rawTarget <= decision.deadbandUpper) {
        decision.threshold = oldThreshold;
        decision.deadbandHeld = true;
        return decision;
    }

    if (decision.rawTarget > oldThreshold) {
        decision.threshold = std::min(decision.rawTarget, decision.riseLimit);
        decision.riseLimited = decision.threshold < decision.rawTarget;
    } else {
        decision.threshold = std::max(decision.rawTarget, decision.fallLimit);
        decision.fallLimited = decision.threshold > decision.rawTarget;
    }
    decision.transitionLimited = decision.riseLimited || decision.fallLimited;
    return decision;
}

} // namespace MapleRuntime

#endif // MRT_MS_HEAP_THRESHOLD_POLICY_H
