// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_HEAP_THRESHOLD_POLICY_H
#define MRT_MS_HEAP_THRESHOLD_POLICY_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

namespace MapleRuntime {

constexpr size_t kMSHeapThresholdHistoryLength = 8;
constexpr size_t kMSHeapThresholdDeadbandDivisor = 10;
constexpr size_t kMSHeapThresholdTransitionDivisor = 8;
constexpr size_t kMSScavengeFallWindows = 8;
constexpr size_t kMSScavengeFallDivisor = 16;
constexpr size_t kMSScavengeColdReuseReserveMultiplier = 2;
constexpr size_t kMSScavengeColdReuseReserveDecayDivisor = 8;
constexpr size_t kMSMarkMinimumRunwayBytes = 4ULL * 1024ULL * 1024ULL;
constexpr long double kMSMarkMaximumRunwayRatio = 0.30L;

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

struct MSStableLiveDecision {
    size_t thresholdBase = 0;
    size_t nextStableLive = 0;
    size_t medianStableLive = 0;
    size_t historySize = 0;
    bool floatingLiveInflated = false;
    bool acceptedSample = false;
    bool hadAcceptedHistory = false;
    bool exceptionalBypass = false;
};

struct MSHeapThresholdState {
    std::array<size_t, kMSHeapThresholdHistoryLength> liveHistory {};
    size_t historySize = 0;
    size_t nextHistoryIndex = 0;
};

struct MSScavengeThresholdDecision {
    size_t heapInUseBytes = 0;
    size_t oldHeapGoalBytes = 0;
    size_t newHeapGoalBytes = 0;
    size_t scaledHeapGoalBytes = 0;
    size_t scaledHeapInUseBytes = 0;
    size_t retainedHeadroomBytes = 0;
    size_t candidateBytes = 0;
    size_t selectedTargetBytes = 0;
    size_t thresholdBytes = 0;
    size_t coldReuseReserveBytes = 0;
    size_t lowerTargetWindows = 0;
    bool fallHeld = false;
    bool fallLimited = false;
};

struct MSScavengeThresholdState {
    size_t thresholdBytes = 0;
    size_t coldReuseReserveBytes = 0;
    size_t lowerTargetWindows = 0;
};

struct MSMarkRunwayDecision {
    size_t heapGoalGapBytes = 0;
    size_t estimatedRunwayBytes = 0;
    size_t minimumRunwayBytes = 0;
    size_t maximumRunwayBytes = 0;
    size_t runwayBytes = 0;
};

inline size_t SaturatingAddMSBytes(size_t lhs, size_t rhs)
{
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        return std::numeric_limits<size_t>::max();
    }
    return lhs + rhs;
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

inline MSMarkRunwayDecision ComputeMSMarkRunway(
    size_t completedMarkAllocatedBytes,
    size_t previousTrueLiveBytes,
    size_t heapGoalBytes)
{
    MSMarkRunwayDecision decision;
    decision.estimatedRunwayBytes = completedMarkAllocatedBytes;
    if (heapGoalBytes <= previousTrueLiveBytes) {
        return decision;
    }

    decision.heapGoalGapBytes = heapGoalBytes - previousTrueLiveBytes;
    decision.minimumRunwayBytes = std::min(
        decision.heapGoalGapBytes, kMSMarkMinimumRunwayBytes);
    decision.maximumRunwayBytes = std::max(
        decision.minimumRunwayBytes,
        ScaleMSHeapThreshold(
            decision.heapGoalGapBytes, kMSMarkMaximumRunwayRatio));
    decision.runwayBytes = std::max(
        decision.minimumRunwayBytes,
        std::min(decision.estimatedRunwayBytes,
                 decision.maximumRunwayBytes));
    return decision;
}

inline bool ShouldTriggerMSMark(
    size_t sweptBytes, size_t runwayBytes, size_t heapGoalBytes)
{
    return sweptBytes >= heapGoalBytes ||
        runwayBytes >= heapGoalBytes - sweptBytes;
}

inline bool IsMSFloatingLiveInflated(size_t markStartInUseBytes,
                                     size_t markEndInUseBytes,
                                     size_t markBitmapLiveBytes)
{
    size_t start = std::max<size_t>(markStartInUseBytes, 1);
    bool grewFourTimes = markEndInUseBytes / start >= 4;
    bool liveTracksGrowth =
        start <= std::numeric_limits<size_t>::max() / 2 &&
        markBitmapLiveBytes > start * 2;
    return grewFourTimes && liveTracksGrowth;
}

inline size_t ComputeMSHeapThresholdMedian(const MSHeapThresholdState& state)
{
    if (state.historySize == 0) {
        return 0;
    }

    std::array<size_t, kMSHeapThresholdHistoryLength> sorted = state.liveHistory;
    std::sort(sorted.begin(), sorted.begin() + state.historySize);
    size_t upperIndex = state.historySize / 2;
    if (state.historySize % 2 != 0) {
        return sorted[upperIndex];
    }
    size_t lower = sorted[upperIndex - 1];
    size_t upper = sorted[upperIndex];
    return lower + (upper - lower) / 2;
}

inline MSStableLiveDecision EvaluateMSStableLive(MSHeapThresholdState& state,
                                                 size_t lastStableLiveBytes,
                                                 size_t markStartInUseBytes,
                                                 size_t markEndInUseBytes,
                                                 size_t markBitmapLiveBytes,
                                                 bool exceptionalBypass)
{
    MSStableLiveDecision decision;
    decision.hadAcceptedHistory = state.historySize != 0;
    decision.historySize = state.historySize;
    decision.exceptionalBypass = exceptionalBypass;
    if (exceptionalBypass) {
        decision.thresholdBase = markBitmapLiveBytes;
        decision.nextStableLive = lastStableLiveBytes;
        return decision;
    }

    decision.floatingLiveInflated = IsMSFloatingLiveInflated(
        markStartInUseBytes, markEndInUseBytes, markBitmapLiveBytes);
    if (decision.floatingLiveInflated) {
        if (state.historySize != 0) {
            decision.medianStableLive = ComputeMSHeapThresholdMedian(state);
            decision.thresholdBase = decision.medianStableLive;
        } else {
            decision.thresholdBase = std::max(lastStableLiveBytes, markStartInUseBytes);
        }
        decision.nextStableLive = lastStableLiveBytes;
        return decision;
    }

    state.liveHistory[state.nextHistoryIndex] = markBitmapLiveBytes;
    state.nextHistoryIndex = (state.nextHistoryIndex + 1) % kMSHeapThresholdHistoryLength;
    state.historySize = std::min(state.historySize + 1, kMSHeapThresholdHistoryLength);
    decision.acceptedSample = true;
    decision.historySize = state.historySize;
    decision.medianStableLive = ComputeMSHeapThresholdMedian(state);
    decision.thresholdBase = decision.medianStableLive;
    decision.nextStableLive = decision.medianStableLive;
    return decision;
}

inline MSHeapThresholdDecision EvaluateMSHeapThreshold(size_t oldThreshold, size_t liveBytes,
                                                       double heapGrowth, size_t minThreshold,
                                                       size_t maxThreshold,
                                                       bool bypassTransition = false)
{
    MSHeapThresholdDecision decision;
    minThreshold = std::min(minThreshold, maxThreshold);
    decision.rawTarget = ScaleMSHeapThreshold(liveBytes, 1.0L + static_cast<long double>(heapGrowth));
    decision.rawTarget = std::max(minThreshold, std::min(decision.rawTarget, maxThreshold));

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

inline MSScavengeThresholdDecision EvaluateMSScavengeThreshold(
    MSScavengeThresholdState& state,
    size_t heapInUseBytes,
    size_t newHeapGoalBytes,
    size_t maxHeapBytes,
    double retainMultiplier,
    double heapGoalMultiplier,
    size_t coldReuseBytes)
{
    MSScavengeThresholdDecision decision;
    decision.heapInUseBytes = heapInUseBytes;
    decision.newHeapGoalBytes = newHeapGoalBytes;
    decision.scaledHeapInUseBytes = heapInUseBytes;

    long double boundedHeapGoalMultiplier = std::max(
        static_cast<long double>(heapGoalMultiplier), 1.0L);
    decision.scaledHeapGoalBytes = ScaleMSHeapThreshold(
        newHeapGoalBytes, boundedHeapGoalMultiplier);

    long double boundedRetainMultiplier = std::max(
        static_cast<long double>(retainMultiplier), 1.0L);
    decision.candidateBytes = ScaleMSHeapThreshold(
        decision.scaledHeapInUseBytes, boundedRetainMultiplier);
    decision.retainedHeadroomBytes =
        decision.candidateBytes >= decision.scaledHeapInUseBytes
        ? decision.candidateBytes - decision.scaledHeapInUseBytes
        : 0;
    // Keep enough hot free memory for mutators to reach the next scheduled GC.
    // A lower target releases pages that the allocator must fault back in before
    // newHeapGoalBytes, producing release/reuse oscillation under steady load.
    size_t reuseReserveSample = coldReuseBytes >
        std::numeric_limits<size_t>::max() / kMSScavengeColdReuseReserveMultiplier
        ? std::numeric_limits<size_t>::max()
        : coldReuseBytes * kMSScavengeColdReuseReserveMultiplier;
    if (coldReuseBytes != 0) {
        state.coldReuseReserveBytes = std::max(
            state.coldReuseReserveBytes, reuseReserveSample);
    } else if (state.coldReuseReserveBytes != 0) {
        state.coldReuseReserveBytes -= CeilDivideMSHeapThreshold(
            state.coldReuseReserveBytes,
            kMSScavengeColdReuseReserveDecayDivisor);
    }
    if (maxHeapBytes != 0) {
        state.coldReuseReserveBytes = std::min(
            state.coldReuseReserveBytes, maxHeapBytes);
    }
    decision.coldReuseReserveBytes = state.coldReuseReserveBytes;

    size_t baseTarget = std::max(
        decision.candidateBytes, decision.scaledHeapGoalBytes);
    decision.selectedTargetBytes = SaturatingAddMSBytes(
        baseTarget, state.coldReuseReserveBytes);
    size_t desiredThreshold = decision.selectedTargetBytes;
    if (maxHeapBytes != 0) {
        desiredThreshold = std::min(desiredThreshold, maxHeapBytes);
    }

    size_t previousThreshold = state.thresholdBytes;
    if (maxHeapBytes != 0) {
        previousThreshold = std::min(previousThreshold, maxHeapBytes);
    }
    if (previousThreshold == 0 || desiredThreshold >= previousThreshold) {
        decision.thresholdBytes = desiredThreshold;
        state.lowerTargetWindows = 0;
    } else {
        ++state.lowerTargetWindows;
        if (state.lowerTargetWindows < kMSScavengeFallWindows) {
            decision.thresholdBytes = previousThreshold;
            decision.fallHeld = true;
        } else {
            size_t maxFall = CeilDivideMSHeapThreshold(
                previousThreshold, kMSScavengeFallDivisor);
            size_t fallLimit = previousThreshold - maxFall;
            decision.thresholdBytes = std::max(desiredThreshold, fallLimit);
            decision.fallLimited = decision.thresholdBytes > desiredThreshold;
            state.lowerTargetWindows = 0;
        }
    }
    state.thresholdBytes = decision.thresholdBytes;
    decision.lowerTargetWindows = state.lowerTargetWindows;
    return decision;
}

} // namespace MapleRuntime

#endif // MRT_MS_HEAP_THRESHOLD_POLICY_H
