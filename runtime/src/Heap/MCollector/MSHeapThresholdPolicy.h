// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_HEAP_THRESHOLD_POLICY_H
#define MRT_MS_HEAP_THRESHOLD_POLICY_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace MapleRuntime {

constexpr size_t kMSHeapThresholdHistoryLength = 8;
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
    size_t gcGoalBytes = 0;
    size_t baseBytes = 0;
    size_t thresholdBytes = 0;
};

// Scavenge uses a low/high retained-heap band instead of releasing every
// Region immediately after it becomes reclaimable. The low watermark keeps
// the capacity used in the just-finished allocation period or the current
// HeapGrow heap goal, whichever is larger. The high watermark is only a
// full allocation runway that decides when background Scavenge starts.
struct MSScavengeWatermarkDecision {
    size_t retainBudgetBytes = 0;
    size_t retainLowBytes = 0;
    size_t retainHighBytes = 0;
    size_t releaseBandBytes = 0;
    bool budgetConstrained = false;
};

constexpr uint64_t kMSScavengeHighDecayIntervalUs = 10000000ULL;

// Low follows the most recent pre-sweep footprint directly. High is a cache
// admission gate and must not fall on every short GC-cycle fluctuation: it
// rises immediately, then gives back no more than one Segment per second.
struct MSScavengeHighWatermarkDecision {
    size_t desiredHighBytes = 0;
    size_t retainHighBytes = 0;
    size_t decayBytes = 0;
    uint64_t decayIntervals = 0;
    bool highRaised = false;
    bool highHeld = false;
    bool highDecayed = false;
};

// HeapGrowth is an internal controller state, seeded once from cjHeapGrowth.
// It deliberately has no extra user-facing tuning knobs: cjHeapSize remains
// the priority, while the controller only trades unused heap headroom for a
// lower normal HEU-GC frequency.
enum class MSAdaptiveHeapGrowthAction : uint8_t {
    HOLD = 0,
    RAISE,
    ROLLBACK,
    LOW_GC_RATIO,
    MEMORY_PRESSURE,
    CRITICAL_PRESSURE,
};

struct MSAdaptiveHeapGrowthState {
    bool initialized = false;
    double effectiveGrowth = 0.0;
    double gcRatioEwma = 0.0;
    double gcWallEwmaUs = 0.0;
    double growthBeforeLastRaise = 0.0;
    double ratioBeforeLastRaise = 0.0;
    double wallBeforeLastRaiseUs = 0.0;
    size_t previousThresholdBaseBytes = 0;
    size_t previousPreSweepInUseBytes = 0;
    bool observingRaise = false;
    uint8_t highRatioStreak = 0;
    uint8_t lowRatioStreak = 0;
    uint8_t stableWorkingSetStreak = 0;
    uint8_t observationCycles = 0;
    uint8_t cooldownCycles = 0;
    uint64_t lastAdjustmentUs = 0;
};

struct MSAdaptiveHeapGrowthInput {
    bool adaptiveEnabled = true;
    double initialGrowth = 0.0;
    double maxGrowth = 0.0;
    size_t thresholdBaseBytes = 0;
    size_t preSweepInUseBytes = 0;
    size_t softLimitBytes = 0;
    uint64_t gcWallUs = 0;
    uint64_t gcPeriodUs = 0;
    uint64_t nowUs = 0;
    bool normalHeu = false;
    bool oom = false;
};

struct MSAdaptiveHeapGrowthDecision {
    double effectiveGrowth = 0.0;
    double gcRatio = 0.0;
    double gcRatioEwma = 0.0;
    double gcWallEwmaUs = 0.0;
    uint8_t cooldownCycles = 0;
    uint8_t stableWorkingSetStreak = 0;
    MSAdaptiveHeapGrowthAction action = MSAdaptiveHeapGrowthAction::HOLD;
    bool bypassTransition = false;
    bool memoryPressure = false;
    bool criticalPressure = false;
    bool requestHighContraction = false;
};

inline const char* MSAdaptiveHeapGrowthActionName(MSAdaptiveHeapGrowthAction action)
{
    switch (action) {
        case MSAdaptiveHeapGrowthAction::HOLD:
            return "hold";
        case MSAdaptiveHeapGrowthAction::RAISE:
            return "raise";
        case MSAdaptiveHeapGrowthAction::ROLLBACK:
            return "rollback";
        case MSAdaptiveHeapGrowthAction::LOW_GC_RATIO:
            return "low_gc_ratio";
        case MSAdaptiveHeapGrowthAction::MEMORY_PRESSURE:
            return "memory_pressure";
        case MSAdaptiveHeapGrowthAction::CRITICAL_PRESSURE:
            return "critical_pressure";
    }
    return "unknown";
}

inline bool IsMSAdaptiveWorkingSetStable(size_t currentBytes, size_t previousBytes)
{
    if (currentBytes == 0 || previousBytes == 0) {
        return false;
    }
    size_t difference = currentBytes >= previousBytes
        ? currentBytes - previousBytes
        : previousBytes - currentBytes;
    return difference <= previousBytes / 10 + 1;
}

struct MSGCThresholdBaseDecision {
    size_t markedLiveBytes = 0;
    size_t gcBaseBytes = 0;
};

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

inline MSAdaptiveHeapGrowthDecision EvaluateMSAdaptiveHeapGrowth(
    MSAdaptiveHeapGrowthState& state, const MSAdaptiveHeapGrowthInput& input)
{
    constexpr double kGrowthStepMultiplier = 1.125;
    constexpr double kTargetGcRatio = 0.35;
    constexpr double kMaximumGrowthStepMultiplier = 2.0;
    constexpr double kRaiseRatio = 0.40;
    constexpr double kHoldRatio = 0.20;
    constexpr uint64_t kMinimumAdjustmentIntervalUs = 2000000ULL;
    constexpr uint8_t kRaiseStreakCycles = 2;
    constexpr uint8_t kObservationCycles = 2;
    constexpr uint8_t kRollbackCooldownCycles = 4;
    constexpr uint8_t kStableWorkingSetCycles = 3;
    constexpr uint8_t kLowRatioContractionCycles = 2;
    constexpr double kRollbackWallIncrease = 1.25;
    constexpr double kRollbackRequiredRatioReduction = 0.90;
    constexpr double kEwmaPreviousWeight = 0.75;
    constexpr double kEwmaSampleWeight = 0.25;

    MSAdaptiveHeapGrowthDecision decision;
    double maxGrowth = std::max(0.0, input.maxGrowth);
    double configuredGrowth = std::min(
        std::max(0.0, input.initialGrowth), maxGrowth);
    if (!state.initialized) {
        state.initialized = true;
        state.effectiveGrowth = configuredGrowth;
    }
    if (!input.adaptiveEnabled) {
        // Static mode is intentionally history-free: cjHeapGrowth is the
        // complete policy, not merely the seed of an internal controller.
        state = MSAdaptiveHeapGrowthState {};
        state.initialized = true;
        state.effectiveGrowth = configuredGrowth;
        decision.effectiveGrowth = configuredGrowth;
        return decision;
    }
    bool cadenceOpen = input.nowUs == 0 || state.lastAdjustmentUs == 0 ||
        (input.nowUs >= state.lastAdjustmentUs &&
         input.nowUs - state.lastAdjustmentUs >= kMinimumAdjustmentIntervalUs);

    // Heap pressure bypasses the normal threshold fall-rate limiter. This is
    // important because the soft budget is a safety constraint, not a tuning
    // suggestion. A zero soft limit means no configured heap budget.
    bool hasSoftLimit = input.softLimitBytes != 0;
    bool criticalPressure = input.oom || (hasSoftLimit &&
        input.preSweepInUseBytes >= input.softLimitBytes - input.softLimitBytes / 10);
    bool memoryPressure = hasSoftLimit &&
        input.preSweepInUseBytes >= input.softLimitBytes - input.softLimitBytes / 5;
    if (criticalPressure) {
        state.effectiveGrowth = 0.0;
        state.highRatioStreak = 0;
        state.observingRaise = false;
        state.observationCycles = 0;
        state.cooldownCycles = 0;
        state.lastAdjustmentUs = input.nowUs;
        decision.action = MSAdaptiveHeapGrowthAction::CRITICAL_PRESSURE;
        decision.bypassTransition = true;
        decision.memoryPressure = true;
        decision.criticalPressure = true;
        decision.requestHighContraction = true;
    } else if (memoryPressure) {
        state.effectiveGrowth = std::max(
            0.0, (1.0 + state.effectiveGrowth) / kGrowthStepMultiplier - 1.0);
        state.highRatioStreak = 0;
        state.observingRaise = false;
        state.observationCycles = 0;
        state.lastAdjustmentUs = input.nowUs;
        decision.action = MSAdaptiveHeapGrowthAction::MEMORY_PRESSURE;
        decision.bypassTransition = true;
        decision.memoryPressure = true;
        decision.requestHighContraction = true;
    } else if (input.normalHeu && input.gcPeriodUs != 0) {
        decision.gcRatio = static_cast<double>(input.gcWallUs) /
            static_cast<double>(input.gcPeriodUs);
        if (state.gcRatioEwma == 0.0) {
            state.gcRatioEwma = decision.gcRatio;
        } else {
            state.gcRatioEwma = state.gcRatioEwma * kEwmaPreviousWeight +
                decision.gcRatio * kEwmaSampleWeight;
        }
        if (state.gcWallEwmaUs == 0.0) {
            state.gcWallEwmaUs = static_cast<double>(input.gcWallUs);
        } else {
            state.gcWallEwmaUs = state.gcWallEwmaUs * kEwmaPreviousWeight +
                static_cast<double>(input.gcWallUs) * kEwmaSampleWeight;
        }

        bool workingSetStable = IsMSAdaptiveWorkingSetStable(
            input.thresholdBaseBytes, state.previousThresholdBaseBytes) &&
            IsMSAdaptiveWorkingSetStable(
                input.preSweepInUseBytes, state.previousPreSweepInUseBytes);
        state.previousThresholdBaseBytes = input.thresholdBaseBytes;
        state.previousPreSweepInUseBytes = input.preSweepInUseBytes;
        if (workingSetStable) {
            if (state.stableWorkingSetStreak == 0) {
                // Discard allocation-ramp samples. They represent bootstrap
                // work with almost no mutator runway, not steady-state GC
                // pressure for the now-stable object graph.
                state.gcRatioEwma = decision.gcRatio;
                state.gcWallEwmaUs = static_cast<double>(input.gcWallUs);
            }
            state.stableWorkingSetStreak = std::min<uint8_t>(
                static_cast<uint8_t>(state.stableWorkingSetStreak + 1),
                kStableWorkingSetCycles);
        } else {
            state.stableWorkingSetStreak = 0;
            state.highRatioStreak = 0;
            state.lowRatioStreak = 0;
            state.observingRaise = false;
            state.observationCycles = 0;
        }

        if (state.stableWorkingSetStreak < kStableWorkingSetCycles) {
            // The first allocation ramp has near-contiguous HEU cycles. Its
            // wall/period ratio is not a steady-state throughput signal.
        } else if (state.observingRaise) {
            ++state.observationCycles;
            if (state.observationCycles >= kObservationCycles) {
                bool slowerGc = state.gcWallEwmaUs >
                    state.wallBeforeLastRaiseUs * kRollbackWallIncrease;
                bool ratioNotImproved = state.gcRatioEwma >
                    state.ratioBeforeLastRaise * kRollbackRequiredRatioReduction;
                state.observationCycles = 0;
                state.observingRaise = false;
                state.highRatioStreak = 0;
                state.lowRatioStreak = 0;
                if (slowerGc && ratioNotImproved) {
                    state.effectiveGrowth = state.growthBeforeLastRaise;
                    state.cooldownCycles = kRollbackCooldownCycles;
                    state.lastAdjustmentUs = input.nowUs;
                    decision.action = MSAdaptiveHeapGrowthAction::ROLLBACK;
                }
            }
        } else if (!cadenceOpen) {
            // GC periods in Case27 can be only tens of milliseconds. Do not
            // turn those adjacent samples into a sequence of conflicting heap
            // adjustments; require a fresh stable streak after the cadence
            // window opens.
            state.highRatioStreak = 0;
            state.lowRatioStreak = 0;
        } else if (state.gcRatioEwma <= kHoldRatio) {
            state.highRatioStreak = 0;
            state.lowRatioStreak = std::min<uint8_t>(
                static_cast<uint8_t>(state.lowRatioStreak + 1),
                kLowRatioContractionCycles);
            if (state.lowRatioStreak >= kLowRatioContractionCycles &&
                state.effectiveGrowth > configuredGrowth) {
                // A normal low-cost observation is a throughput-tuning signal,
                // not heap pressure. Give back only one bounded step so a
                // transient low-ratio interval cannot discard the reusable
                // Region runway built by the preceding high-cost interval.
                state.effectiveGrowth = std::max(configuredGrowth,
                    (1.0 + state.effectiveGrowth) /
                        kGrowthStepMultiplier - 1.0);
                state.lowRatioStreak = 0;
                state.lastAdjustmentUs = input.nowUs;
                decision.action = MSAdaptiveHeapGrowthAction::LOW_GC_RATIO;
            }
        } else if (state.cooldownCycles != 0) {
            --state.cooldownCycles;
            state.highRatioStreak = 0;
            state.lowRatioStreak = 0;
        } else if (state.gcRatioEwma > kRaiseRatio) {
            state.highRatioStreak = std::min<uint8_t>(
                static_cast<uint8_t>(state.highRatioStreak + 1), kRaiseStreakCycles);
            if (state.highRatioStreak >= kRaiseStreakCycles) {
                double ratioScale = std::min(kMaximumGrowthStepMultiplier,
                    std::max(1.0, state.gcRatioEwma / kTargetGcRatio));
                double raisedGrowth = std::min(maxGrowth,
                    (1.0 + state.effectiveGrowth) * ratioScale - 1.0);
                state.highRatioStreak = 0;
                state.lowRatioStreak = 0;
                if (raisedGrowth > state.effectiveGrowth) {
                    state.growthBeforeLastRaise = state.effectiveGrowth;
                    state.ratioBeforeLastRaise = state.gcRatioEwma;
                    state.wallBeforeLastRaiseUs = state.gcWallEwmaUs;
                    state.effectiveGrowth = raisedGrowth;
                    state.observationCycles = 0;
                    state.observingRaise = true;
                    state.lastAdjustmentUs = input.nowUs;
                    decision.action = MSAdaptiveHeapGrowthAction::RAISE;
                }
            }
        } else {
            state.highRatioStreak = 0;
            state.lowRatioStreak = 0;
        }
    }

    decision.effectiveGrowth = state.effectiveGrowth;
    decision.gcRatioEwma = state.gcRatioEwma;
    decision.gcWallEwmaUs = state.gcWallEwmaUs;
    decision.cooldownCycles = state.cooldownCycles;
    decision.stableWorkingSetStreak = state.stableWorkingSetStreak;
    return decision;
}

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
        decision.retainBudgetBytes = std::numeric_limits<size_t>::max();
        decision.retainLowBytes = std::max(heapGoalBytes, preSweepInUseBytes);
        decision.releaseBandBytes = minimumBand;
        decision.retainHighBytes = std::max(
            SaturatingAddMSBytes(decision.retainLowBytes, decision.releaseBandBytes),
            SaturatingMultiplyMSBytes(heapGoalBytes, 3) / 2);
        return decision;
    }

    size_t emergencyReserve = std::max(
        SaturatingMultiplyMSBytes(segmentBytes, 2), softLimitBytes / 16);
    decision.retainBudgetBytes = emergencyReserve >= softLimitBytes
        ? 0
        : softLimitBytes - emergencyReserve;
    size_t boundedHeapGoal = std::min(heapGoalBytes, decision.retainBudgetBytes);
    decision.retainLowBytes = std::max(preSweepInUseBytes, boundedHeapGoal);
    decision.releaseBandBytes = minimumBand;
    size_t stableCacheFloor = SaturatingMultiplyMSBytes(boundedHeapGoal, 3) / 2;
    size_t lowWatermarkRunway = SaturatingAddMSBytes(
        decision.retainLowBytes, decision.releaseBandBytes);
    size_t desiredHighBytes = std::max(lowWatermarkRunway, stableCacheFloor);
    decision.retainHighBytes = std::max(
        decision.retainLowBytes,
        std::min(decision.retainBudgetBytes, desiredHighBytes));
    decision.budgetConstrained = heapGoalBytes > decision.retainBudgetBytes ||
        preSweepInUseBytes > decision.retainBudgetBytes;
    return decision;
}

inline MSScavengeHighWatermarkDecision StabilizeMSScavengeHighWatermark(
    size_t desiredHighBytes, size_t retainLowBytes,
    size_t previousHighBytes, uint64_t elapsedDecayUs, size_t segmentBytes,
    bool forceDecrease = false)
{
    MSScavengeHighWatermarkDecision decision;
    decision.desiredHighBytes = std::max(desiredHighBytes, retainLowBytes);
    if (previousHighBytes == 0 || decision.desiredHighBytes >= previousHighBytes) {
        decision.retainHighBytes = decision.desiredHighBytes;
        decision.highRaised = previousHighBytes != 0 &&
            decision.desiredHighBytes > previousHighBytes;
        return decision;
    }

    if (forceDecrease) {
        decision.retainHighBytes = decision.desiredHighBytes;
        decision.highDecayed = true;
        return decision;
    }

    decision.decayIntervals = elapsedDecayUs / kMSScavengeHighDecayIntervalUs;
    if (decision.decayIntervals == 0) {
        decision.retainHighBytes = previousHighBytes;
        decision.highHeld = true;
        return decision;
    }

    size_t decayStepCount = decision.decayIntervals >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max())
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(decision.decayIntervals);
    decision.decayBytes = SaturatingMultiplyMSBytes(
        std::max<size_t>(segmentBytes, 1), decayStepCount);
    size_t decayedHighBytes = previousHighBytes > decision.decayBytes
        ? previousHighBytes - decision.decayBytes
        : 0;
    decision.retainHighBytes = std::max(
        retainLowBytes,
        std::max(decision.desiredHighBytes, decayedHighBytes));
    decision.highDecayed = decision.retainHighBytes < previousHighBytes;
    decision.highHeld = !decision.highDecayed;
    return decision;
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

inline MSGCThresholdBaseDecision ComputeMSGCThresholdBase(
    size_t markedLiveBytes)
{
    MSGCThresholdBaseDecision decision;
    decision.markedLiveBytes = markedLiveBytes;
    decision.gcBaseBytes = decision.markedLiveBytes;
    return decision;
}

inline bool ShouldTriggerMSMark(size_t sweptBytes, size_t heapGoalBytes)
{
    return sweptBytes >= heapGoalBytes;
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
    size_t gcGoalBytes,
    size_t maxHeapBytes,
    double scavengeGrowth)
{
    MSScavengeThresholdDecision decision;
    decision.gcGoalBytes = gcGoalBytes;
    decision.baseBytes = gcGoalBytes;
    long double boundedGrowth = std::max(
        static_cast<long double>(scavengeGrowth), 1.0L);
    decision.thresholdBytes = ScaleMSHeapThreshold(
        decision.baseBytes, boundedGrowth);
    if (maxHeapBytes != 0) {
        decision.thresholdBytes = std::min(
            decision.thresholdBytes, maxHeapBytes);
    }
    return decision;
}

// Static MSGC mode preserves the user-facing cjScavengeGrowth contract. The
// configured multiplier controls the high wake-up watermark directly, while
// the low target remains the just-observed in-use footprint.  The RegionSpace
// release path still clamps low to its current in-use bytes before releasing.
inline MSScavengeWatermarkDecision EvaluateMSStaticScavengeWatermarks(
    size_t heapGoalBytes, size_t preSweepInUseBytes,
    size_t softLimitBytes, double scavengeGrowth)
{
    MSScavengeWatermarkDecision decision;
    if (softLimitBytes == 0) {
        decision.retainBudgetBytes = std::numeric_limits<size_t>::max();
    } else {
        size_t emergencyReserve = softLimitBytes / 16;
        decision.retainBudgetBytes = emergencyReserve >= softLimitBytes
            ? 0
            : softLimitBytes - emergencyReserve;
    }
    decision.retainLowBytes = std::min(preSweepInUseBytes,
                                       decision.retainBudgetBytes);
    MSScavengeThresholdDecision staticThreshold = EvaluateMSScavengeThreshold(
        heapGoalBytes, decision.retainBudgetBytes, scavengeGrowth);
    decision.retainHighBytes = std::max(decision.retainLowBytes,
                                        staticThreshold.thresholdBytes);
    decision.releaseBandBytes = decision.retainHighBytes - decision.retainLowBytes;
    decision.budgetConstrained = heapGoalBytes > decision.retainBudgetBytes ||
        preSweepInUseBytes > decision.retainBudgetBytes;
    return decision;
}

} // namespace MapleRuntime

#endif // MRT_MS_HEAP_THRESHOLD_POLICY_H
