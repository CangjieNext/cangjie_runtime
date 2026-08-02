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
constexpr size_t kMSScavengeHistoryLength = 64;
constexpr size_t kMSScavengeLowCyclesBeforeDecrease = 32;
constexpr size_t kMSScavengeDecreaseDivisor = 64;

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

struct MSScavengeThresholdState {
    std::array<size_t, kMSScavengeHistoryLength> inUseHistory {};
    std::array<size_t, kMSScavengeHistoryLength> burstHistory {};
    size_t historySize = 0;
    size_t nextHistoryIndex = 0;
    size_t residentReserveBytes = 0;
    size_t noColdReuseCycles = 0;
};

struct MSScavengeThresholdDecision {
    size_t currentBurstBytes = 0;
    size_t recentPeakInUseBytes = 0;
    size_t recentBurstBytes = 0;
    size_t coldReuseBytes = 0;
    size_t residentReserveBytes = 0;
    size_t candidateBytes = 0;
    size_t thresholdBytes = 0;
    size_t noColdReuseCycles = 0;
    bool decreased = false;
    bool thresholdChangeLimited = false;
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
    size_t markStartInUseBytes,
    size_t markEndInUseBytes,
    size_t coldReuseBytes,
    size_t maxHeapBytes,
    size_t oldThresholdBytes,
    size_t segmentBytes)
{
    MSScavengeThresholdDecision decision;
    decision.currentBurstBytes = markEndInUseBytes > markStartInUseBytes
        ? markEndInUseBytes - markStartInUseBytes
        : 0;

    state.inUseHistory[state.nextHistoryIndex] = markEndInUseBytes;
    state.burstHistory[state.nextHistoryIndex] = decision.currentBurstBytes;
    state.nextHistoryIndex = (state.nextHistoryIndex + 1) % kMSScavengeHistoryLength;
    state.historySize = std::min(state.historySize + 1, kMSScavengeHistoryLength);

    for (size_t i = 0; i < state.historySize; ++i) {
        decision.recentPeakInUseBytes =
            std::max(decision.recentPeakInUseBytes, state.inUseHistory[i]);
        decision.recentBurstBytes = std::max(decision.recentBurstBytes, state.burstHistory[i]);
    }

    state.residentReserveBytes =
        std::max(state.residentReserveBytes, decision.recentBurstBytes);
    decision.coldReuseBytes = coldReuseBytes;
    if (coldReuseBytes != 0) {
        state.residentReserveBytes =
            SaturatingAddMSBytes(state.residentReserveBytes, coldReuseBytes);
        if (maxHeapBytes != 0) {
            state.residentReserveBytes =
                std::min(state.residentReserveBytes, maxHeapBytes);
        }
        state.noColdReuseCycles = 0;
    } else {
        state.noColdReuseCycles = std::min(
            state.noColdReuseCycles + 1, kMSScavengeLowCyclesBeforeDecrease);
        if (state.noColdReuseCycles >= kMSScavengeLowCyclesBeforeDecrease &&
            state.residentReserveBytes > decision.recentBurstBytes) {
            size_t gapBytes =
                state.residentReserveBytes - decision.recentBurstBytes;
            size_t decreaseBytes =
                gapBytes / kMSScavengeDecreaseDivisor +
                static_cast<size_t>(gapBytes % kMSScavengeDecreaseDivisor != 0);
            state.residentReserveBytes -= decreaseBytes;
            decision.decreased = true;
        }
    }

    decision.residentReserveBytes = state.residentReserveBytes;
    decision.noColdReuseCycles = state.noColdReuseCycles;
    decision.candidateBytes =
        SaturatingAddMSBytes(decision.recentPeakInUseBytes, decision.residentReserveBytes);
    if (maxHeapBytes != 0) {
        decision.candidateBytes = std::min(decision.candidateBytes, maxHeapBytes);
    }
    decision.thresholdBytes = decision.candidateBytes;
    if (oldThresholdBytes != 0 && segmentBytes != 0) {
        if (decision.candidateBytes > oldThresholdBytes) {
            size_t riseLimit = SaturatingAddMSBytes(oldThresholdBytes, segmentBytes);
            if (decision.candidateBytes > riseLimit) {
                decision.thresholdBytes = riseLimit;
                decision.thresholdChangeLimited = true;
            }
        } else if (decision.candidateBytes < oldThresholdBytes) {
            size_t decreaseLimit =
                oldThresholdBytes > segmentBytes ? oldThresholdBytes - segmentBytes : 0;
            if (decision.candidateBytes < decreaseLimit) {
                decision.thresholdBytes = decreaseLimit;
                decision.thresholdChangeLimited = true;
            }
        }
    }
    if (maxHeapBytes != 0) {
        decision.thresholdBytes = std::min(decision.thresholdBytes, maxHeapBytes);
    }
    return decision;
}

} // namespace MapleRuntime

#endif // MRT_MS_HEAP_THRESHOLD_POLICY_H
