// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_ADAPTIVE_HEAP_GROWTH_POLICY_H
#define MRT_MS_ADAPTIVE_HEAP_GROWTH_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

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
    bool bypassTransition = false;
    bool requestHighContraction = false;
};

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
        decision.bypassTransition = true;
        decision.requestHighContraction = true;
    } else if (memoryPressure) {
        state.effectiveGrowth = std::max(
            0.0, (1.0 + state.effectiveGrowth) / kGrowthStepMultiplier - 1.0);
        state.highRatioStreak = 0;
        state.observingRaise = false;
        state.observationCycles = 0;
        state.lastAdjustmentUs = input.nowUs;
        decision.bypassTransition = true;
        decision.requestHighContraction = true;
    } else if (input.normalHeu && input.gcPeriodUs != 0) {
        double gcRatio = static_cast<double>(input.gcWallUs) /
            static_cast<double>(input.gcPeriodUs);
        if (state.gcRatioEwma == 0.0) {
            state.gcRatioEwma = gcRatio;
        } else {
            state.gcRatioEwma = state.gcRatioEwma * kEwmaPreviousWeight +
                gcRatio * kEwmaSampleWeight;
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
                state.gcRatioEwma = gcRatio;
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
                }
            }
        } else {
            state.highRatioStreak = 0;
            state.lowRatioStreak = 0;
        }
    }

    decision.effectiveGrowth = state.effectiveGrowth;
    return decision;
}

} // namespace MapleRuntime

#endif // MRT_MS_ADAPTIVE_HEAP_GROWTH_POLICY_H
