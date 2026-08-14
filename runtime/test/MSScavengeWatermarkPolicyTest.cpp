// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "src/Heap/MCollector/MSAdaptiveHeapGrowthPolicy.h"
#include "src/Heap/MCollector/MSScavengeWatermarkPolicy.h"
#include "src/Heap/MCollector/MSMemoryLimitPolicy.h"

namespace {

void ExpectEqual(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n", message, expected, actual);
        std::abort();
    }
}

void ExpectTrue(bool actual, const char* message)
{
    if (!actual) {
        std::fprintf(stderr, "%s: expected=true\n", message);
        std::abort();
    }
}

void ExpectFalse(bool actual, const char* message)
{
    if (actual) {
        std::fprintf(stderr, "%s: expected=false\n", message);
        std::abort();
    }
}

void ExpectNear(double actual, double expected, double tolerance, const char* message)
{
    if (std::fabs(actual - expected) > tolerance) {
        std::fprintf(stderr, "%s: expected=%.6f actual=%.6f\n", message, expected, actual);
        std::abort();
    }
}

void TestPreSweepFootprintSetsRetainLow()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeWatermarkDecision decision =
        MapleRuntime::EvaluateMSScavengeWatermarks(
            240 * mib, 300 * mib, 512 * mib, 16 * mib);

    ExpectEqual(decision.retainLowBytes, 300 * mib,
                "pre-sweep footprint must prevent immediate cache release");
    ExpectEqual(decision.retainHighBytes, 360 * mib,
                "the high watermark must retain the stable 1.5-goal cache floor");
}

void TestHeapGrowGoalSetsRetainLowWhenLarger()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeWatermarkDecision decision =
        MapleRuntime::EvaluateMSScavengeWatermarks(
            240 * mib, 180 * mib, 512 * mib, 16 * mib);

    ExpectEqual(decision.retainLowBytes, 240 * mib,
                "HeapGrow goal must retain reusable capacity above active usage");
    ExpectEqual(decision.retainHighBytes, 360 * mib,
                "the normal high watermark must retain the bounded cache floor");
}

void TestScavengeHighUsesStableCacheFloor()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeWatermarkDecision decision =
        MapleRuntime::EvaluateMSScavengeWatermarks(
            240 * mib, 300 * mib, 512 * mib, 16 * mib);

    ExpectEqual(decision.retainLowBytes, 300 * mib,
                "pre-sweep footprint must remain the Scavenge low watermark");
    ExpectEqual(decision.retainHighBytes, 360 * mib,
                "the stable cache floor must coexist with the one-Segment low runway");
}

void TestScavengeHighCapsTransientPreSweepRunway()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeWatermarkDecision decision =
        MapleRuntime::EvaluateMSScavengeWatermarks(
            240 * mib, 600 * mib, 2048 * mib, 16 * mib);

    ExpectEqual(decision.retainLowBytes, 600 * mib,
                "pre-sweep footprint must remain the Scavenge low watermark");
    ExpectEqual(decision.retainHighBytes, 616 * mib,
                "a transient high footprint must add only one Segment above low");
}

void TestStaticScavengeUsesConfiguredGrowth()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeWatermarkDecision decision =
        MapleRuntime::EvaluateMSStaticScavengeWatermarks(
            240 * mib, 180 * mib, 2048 * mib, 2.0);

    ExpectEqual(decision.retainLowBytes, 180 * mib,
                "static Scavenge must retain the observed in-use footprint");
    ExpectEqual(decision.retainHighBytes, 480 * mib,
                "static Scavenge must use cjScavengeGrowth as its high watermark");
}

void TestBudgetCapsBothWatermarks()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeWatermarkDecision decision =
        MapleRuntime::EvaluateMSScavengeWatermarks(
            600 * mib, 420 * mib, 512 * mib, 16 * mib);

    ExpectEqual(decision.retainLowBytes, 480 * mib,
                "HeapGrow goal must be capped by the retain budget");
    ExpectEqual(decision.retainHighBytes, 480 * mib,
                "no release band remains when low reaches budget");
}

void TestScavengeReleaseStartsOnlyAboveHighWatermark()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    ExpectEqual(
        MapleRuntime::ComputeMSScavengeWatermarkReleaseGap(
            332 * mib, 80 * mib, 300 * mib, 332 * mib),
        0,
        "retained bytes at high watermark must not start Scavenge");
    ExpectEqual(
        MapleRuntime::ComputeMSScavengeWatermarkReleaseGap(
            350 * mib, 80 * mib, 300 * mib, 332 * mib),
        50 * mib,
        "Scavenge must release the gap down to low once high is crossed");
    ExpectEqual(
        MapleRuntime::ComputeMSScavengeWatermarkReleaseGap(
            350 * mib, 20 * mib, 300 * mib, 332 * mib),
        20 * mib,
        "release must remain limited by reclaimable bytes");
}

void TestOlderFreeEpochsWinScavengePasses()
{
    ExpectTrue(
        MapleRuntime::HasMSScavengeEpochAge(12, 8, 4),
        "a four-epoch-old Region must be eligible for the oldest pass");
    ExpectFalse(
        MapleRuntime::HasMSScavengeEpochAge(12, 10, 4),
        "a two-epoch-old Region must not bypass the oldest pass");
    ExpectTrue(
        MapleRuntime::HasMSScavengeEpochAge(2, 0xfffffffeU, 4),
        "epoch age must remain correct across uint32 wraparound");
}

void TestRetainHighHoldsBeforeItsDecayInterval()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeHighWatermarkDecision decision =
        MapleRuntime::StabilizeMSScavengeHighWatermark(
            540 * mib, 300 * mib, 600 * mib,
            999999, 16 * mib);

    ExpectEqual(decision.retainHighBytes, 600 * mib,
                "a lower desired high must not fall before one second elapsed");
}

void TestRetainHighDecaysOneSegmentPerTenSeconds()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeHighWatermarkDecision decision =
        MapleRuntime::StabilizeMSScavengeHighWatermark(
            540 * mib, 300 * mib, 600 * mib,
            10000000, 16 * mib);

    ExpectEqual(decision.retainHighBytes, 584 * mib,
                "the high watermark must decay by exactly one Segment per ten seconds");
}

void TestRetainHighRisesImmediately()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeHighWatermarkDecision decision =
        MapleRuntime::StabilizeMSScavengeHighWatermark(
            660 * mib, 300 * mib, 600 * mib,
            0, 16 * mib);

    ExpectEqual(decision.retainHighBytes, 660 * mib,
                "an increased working set must raise high immediately");
}

void TestRetainHighContractsImmediatelyForAdaptiveDownshift()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeHighWatermarkDecision decision =
        MapleRuntime::StabilizeMSScavengeHighWatermark(
            420 * mib, 300 * mib, 600 * mib,
            0, 16 * mib, true);

    ExpectEqual(decision.retainHighBytes, 420 * mib,
                "an adaptive HeapGrow downshift must contract high immediately");
}

MapleRuntime::MSAdaptiveHeapGrowthInput MakeAdaptiveGrowthInput(
    double initialGrowth, size_t preSweepInUseBytes, size_t softLimitBytes,
    uint64_t gcWallUs, uint64_t gcPeriodUs)
{
    MapleRuntime::MSAdaptiveHeapGrowthInput input;
    input.initialGrowth = initialGrowth;
    input.maxGrowth = 20.0;
    input.thresholdBaseBytes = 100;
    input.preSweepInUseBytes = preSweepInUseBytes;
    input.softLimitBytes = softLimitBytes;
    input.gcWallUs = gcWallUs;
    input.gcPeriodUs = gcPeriodUs;
    input.normalHeu = true;
    return input;
}

void WarmAdaptiveWorkingSet(MapleRuntime::MSAdaptiveHeapGrowthState& state)
{
    for (size_t i = 0; i < 4; ++i) {
        (void)MapleRuntime::EvaluateMSAdaptiveHeapGrowth(
            state, MakeAdaptiveGrowthInput(1.0, 100, 1000, 100, 1000));
    }
}

MapleRuntime::MSAdaptiveHeapGrowthDecision RaiseAdaptiveGrowth(
    MapleRuntime::MSAdaptiveHeapGrowthState& state)
{
    MapleRuntime::MSAdaptiveHeapGrowthDecision decision;
    for (size_t i = 0; i < 8; ++i) {
        double previousGrowth = state.effectiveGrowth;
        decision = MapleRuntime::EvaluateMSAdaptiveHeapGrowth(
            state, MakeAdaptiveGrowthInput(1.0, 100, 1000, 1000, 1000));
        if (decision.effectiveGrowth > previousGrowth) {
            return decision;
        }
    }
    return decision;
}

void TestAdaptiveGrowthWaitsForStableWorkingSet()
{
    MapleRuntime::MSAdaptiveHeapGrowthState state;
    for (size_t i = 0; i < 4; ++i) {
        MapleRuntime::MSAdaptiveHeapGrowthDecision decision =
            MapleRuntime::EvaluateMSAdaptiveHeapGrowth(
                state, MakeAdaptiveGrowthInput(1.0, 100, 1000, 600, 1000));
        ExpectNear(decision.effectiveGrowth, 1.0, 0.000001,
                   "startup allocation growth must not raise HeapGrow");
    }

    MapleRuntime::MSAdaptiveHeapGrowthDecision decision =
        MapleRuntime::EvaluateMSAdaptiveHeapGrowth(
            state, MakeAdaptiveGrowthInput(1.0, 100, 1000, 600, 1000));
    ExpectNear(decision.effectiveGrowth, 17.0 / 7.0, 0.000001,
               "a raise must proportionally target the 35-percent GC budget");
}

void TestDisabledAdaptiveGrowthKeepsConfiguredValue()
{
    MapleRuntime::MSAdaptiveHeapGrowthState state;
    MapleRuntime::MSAdaptiveHeapGrowthInput input =
        MakeAdaptiveGrowthInput(1.0, 100, 1000, 900, 1000);
    input.adaptiveEnabled = false;
    for (size_t i = 0; i < 8; ++i) {
        MapleRuntime::MSAdaptiveHeapGrowthDecision decision =
            MapleRuntime::EvaluateMSAdaptiveHeapGrowth(state, input);
        ExpectNear(decision.effectiveGrowth, 1.0, 0.000001,
                   "disabled adaptive mode must keep cjHeapGrowth unchanged");
    }
}

void TestAdaptiveGrowthRaisesProportionallyToSustainedCost()
{
    MapleRuntime::MSAdaptiveHeapGrowthState state;
    MapleRuntime::MSAdaptiveHeapGrowthDecision decision;
    for (size_t i = 0; i < 5; ++i) {
        decision = MapleRuntime::EvaluateMSAdaptiveHeapGrowth(
            state, MakeAdaptiveGrowthInput(1.0, 100, 1000, 700, 1000));
    }

    ExpectNear(decision.effectiveGrowth, 3.0, 0.000001,
               "the raise must proportionally target the 35-percent GC budget");
}

void TestAdaptiveGrowthWaitsForAdjustmentInterval()
{
    MapleRuntime::MSAdaptiveHeapGrowthState state;
    state.initialized = true;
    state.effectiveGrowth = 1.0;
    state.previousThresholdBaseBytes = 100;
    state.previousPreSweepInUseBytes = 100;
    state.stableWorkingSetStreak = 3;
    state.lastAdjustmentUs = 1000000;

    MapleRuntime::MSAdaptiveHeapGrowthInput input =
        MakeAdaptiveGrowthInput(1.0, 100, 1000, 1000, 1000);
    input.nowUs = 2000000;
    for (size_t i = 0; i < 2; ++i) {
        MapleRuntime::MSAdaptiveHeapGrowthDecision decision =
            MapleRuntime::EvaluateMSAdaptiveHeapGrowth(state, input);
        ExpectNear(decision.effectiveGrowth, 1.0, 0.000001,
                   "a one-second-old adjustment must block another HeapGrow change");
    }

    input.nowUs = 3000000;
    (void)MapleRuntime::EvaluateMSAdaptiveHeapGrowth(state, input);
    MapleRuntime::MSAdaptiveHeapGrowthDecision decision =
        MapleRuntime::EvaluateMSAdaptiveHeapGrowth(state, input);
    ExpectNear(decision.effectiveGrowth, 3.0, 0.000001,
               "the controller must allow a new adjustment after two seconds");
}

void TestAdaptiveGrowthDropsStartupRatioWhenWorkingSetStabilizes()
{
    MapleRuntime::MSAdaptiveHeapGrowthState state;
    for (size_t bytes : {100U, 200U, 300U, 400U}) {
        MapleRuntime::MSAdaptiveHeapGrowthInput input =
            MakeAdaptiveGrowthInput(1.0, bytes, 1000, 600, 1000);
        input.thresholdBaseBytes = bytes;
        (void)MapleRuntime::EvaluateMSAdaptiveHeapGrowth(state, input);
    }

    MapleRuntime::MSAdaptiveHeapGrowthDecision decision;
    for (size_t i = 0; i < 4; ++i) {
        MapleRuntime::MSAdaptiveHeapGrowthInput input =
            MakeAdaptiveGrowthInput(1.0, 400, 1000, 350, 1000);
        input.thresholdBaseBytes = 400;
        decision = MapleRuntime::EvaluateMSAdaptiveHeapGrowth(state, input);
    }
    ExpectNear(decision.effectiveGrowth, 1.0, 0.000001,
                "a steady 35-percent ratio must preserve configured HeapGrow");
}

void TestAdaptiveGrowthRollsBackAnUnproductiveRaise()
{
    MapleRuntime::MSAdaptiveHeapGrowthState state;
    WarmAdaptiveWorkingSet(state);
    MapleRuntime::MSAdaptiveHeapGrowthDecision raised = RaiseAdaptiveGrowth(state);
    ExpectTrue(raised.effectiveGrowth > 1.0,
               "the setup must produce one HeapGrow increase");
    (void)MapleRuntime::EvaluateMSAdaptiveHeapGrowth(
        state, MakeAdaptiveGrowthInput(1.0, 100, 1000, 1200, 1200));
    MapleRuntime::MSAdaptiveHeapGrowthDecision decision =
        MapleRuntime::EvaluateMSAdaptiveHeapGrowth(
            state, MakeAdaptiveGrowthInput(1.0, 100, 1000, 1200, 1200));

    ExpectNear(decision.effectiveGrowth, 1.0, 0.000001,
               "rollback must restore the previous effective growth");
    ExpectEqual(state.cooldownCycles, 4,
                "rollback must install a four-cycle raise cooldown");
}

void TestAdaptiveGrowthPrioritizesHeapPressure()
{
    MapleRuntime::MSAdaptiveHeapGrowthState pressureState;
    MapleRuntime::MSAdaptiveHeapGrowthDecision pressure =
        MapleRuntime::EvaluateMSAdaptiveHeapGrowth(
            pressureState, MakeAdaptiveGrowthInput(10.0, 850, 1000, 0, 0));
    ExpectNear(pressure.effectiveGrowth, 11.0 / 1.125 - 1.0, 0.000001,
               "memory pressure must take one inverse 12.5-percent multiplier step");
    ExpectTrue(pressure.bypassTransition,
               "memory-pressure threshold decreases must bypass normal fall limiting");

    MapleRuntime::MSAdaptiveHeapGrowthState criticalState;
    MapleRuntime::MSAdaptiveHeapGrowthDecision critical =
        MapleRuntime::EvaluateMSAdaptiveHeapGrowth(
            criticalState, MakeAdaptiveGrowthInput(10.0, 900, 1000, 0, 0));
    ExpectNear(critical.effectiveGrowth, 0.0, 0.000001,
               "critical heap pressure must reset to the minimum growth");
    ExpectTrue(critical.bypassTransition,
               "critical pressure must bypass normal fall limiting");
    ExpectTrue(critical.requestHighContraction,
               "critical pressure must request immediate Scavenge cache contraction");
}

void TestAdaptiveGrowthContractsGraduallyWithoutHighContraction()
{
    MapleRuntime::MSAdaptiveHeapGrowthState state;
    WarmAdaptiveWorkingSet(state);
    MapleRuntime::MSAdaptiveHeapGrowthDecision raised = RaiseAdaptiveGrowth(state);
    ExpectTrue(raised.effectiveGrowth > 1.0,
               "the setup must produce one HeapGrow increase");

    for (size_t i = 0; i < 2; ++i) {
        (void)MapleRuntime::EvaluateMSAdaptiveHeapGrowth(
            state, MakeAdaptiveGrowthInput(1.0, 100, 1000, 400, 1000));
    }
    double growthBeforeLowRatioContraction = state.effectiveGrowth;
    MapleRuntime::MSAdaptiveHeapGrowthDecision decision;
    for (size_t i = 0; i < 10; ++i) {
        decision = MapleRuntime::EvaluateMSAdaptiveHeapGrowth(
            state, MakeAdaptiveGrowthInput(1.0, 100, 1000, 0, 1000));
        if (decision.effectiveGrowth < growthBeforeLowRatioContraction) {
            break;
        }
    }
    ExpectNear(decision.effectiveGrowth,
               std::max(1.0, (1.0 + growthBeforeLowRatioContraction) / 1.125 - 1.0),
               0.000001,
               "normal low-cost convergence must give back only one 12.5-percent step");
    ExpectFalse(decision.requestHighContraction,
                "a normal HeapGrow downshift must preserve the Scavenge high cache");
}

} // namespace

int main()
{
    TestPreSweepFootprintSetsRetainLow();
    TestHeapGrowGoalSetsRetainLowWhenLarger();
    TestScavengeHighUsesStableCacheFloor();
    TestScavengeHighCapsTransientPreSweepRunway();
    TestStaticScavengeUsesConfiguredGrowth();
    TestBudgetCapsBothWatermarks();
    TestScavengeReleaseStartsOnlyAboveHighWatermark();
    TestOlderFreeEpochsWinScavengePasses();
    TestRetainHighHoldsBeforeItsDecayInterval();
    TestRetainHighDecaysOneSegmentPerTenSeconds();
    TestRetainHighRisesImmediately();
    TestRetainHighContractsImmediatelyForAdaptiveDownshift();
    TestAdaptiveGrowthWaitsForStableWorkingSet();
    TestDisabledAdaptiveGrowthKeepsConfiguredValue();
    TestAdaptiveGrowthRaisesProportionallyToSustainedCost();
    TestAdaptiveGrowthWaitsForAdjustmentInterval();
    TestAdaptiveGrowthDropsStartupRatioWhenWorkingSetStabilizes();
    TestAdaptiveGrowthRollsBackAnUnproductiveRaise();
    TestAdaptiveGrowthPrioritizesHeapPressure();
    TestAdaptiveGrowthContractsGraduallyWithoutHighContraction();
    return 0;
}
