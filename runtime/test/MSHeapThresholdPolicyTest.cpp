#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSHeapThresholdPolicy.h"

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
        std::fprintf(stderr, "%s: expected=true actual=false\n", message);
        std::abort();
    }
}

void ExpectFalse(bool actual, const char* message)
{
    if (actual) {
        std::fprintf(stderr, "%s: expected=false actual=true\n", message);
        std::abort();
    }
}

MapleRuntime::MSStableLiveDecision AcceptLiveSample(
    MapleRuntime::MSHeapThresholdState& state, size_t liveBytes)
{
    return MapleRuntime::EvaluateMSStableLive(
        state, 0, liveBytes, liveBytes, liveBytes, false);
}

void TestStableLiveHistoryUsesAvailableMedian()
{
    MapleRuntime::MSHeapThresholdState state;
    MapleRuntime::MSStableLiveDecision first = AcceptLiveSample(state, 100);
    ExpectFalse(first.hadAcceptedHistory,
                "the first accepted sample must report an empty prior history");
    ExpectTrue(first.acceptedSample, "a normal live sample must enter history");
    ExpectEqual(first.historySize, 1, "the first accepted sample must establish history");
    ExpectEqual(first.medianStableLive, 100, "one sample must be its own median");

    MapleRuntime::MSStableLiveDecision even = AcceptLiveSample(state, 300);
    ExpectTrue(even.hadAcceptedHistory,
               "the second accepted sample must see established history");
    ExpectEqual(even.historySize, 2, "partial history must expose its current size");
    ExpectEqual(even.medianStableLive, 200,
                "an even median must average the two middle values");

    MapleRuntime::MSStableLiveDecision odd = AcceptLiveSample(state, 200);
    ExpectEqual(odd.historySize, 3, "all partial-history samples must participate");
    ExpectEqual(odd.medianStableLive, 200, "an odd median must select the middle value");
    ExpectEqual(odd.thresholdBase, 200, "normal threshold base must use the history median");
    ExpectEqual(odd.nextStableLive, 200, "reported stable live must track the history median");
}

void TestStableLiveEvenMedianDoesNotOverflow()
{
    MapleRuntime::MSHeapThresholdState state;
    constexpr size_t maximum = static_cast<size_t>(-1);
    (void)AcceptLiveSample(state, maximum - 2);
    MapleRuntime::MSStableLiveDecision decision = AcceptLiveSample(state, maximum);

    ExpectEqual(decision.medianStableLive, maximum - 1,
                "the two middle values must be averaged without overflow");
}

void TestStableLiveHistoryEvictsOldestSampleAfterWrap()
{
    MapleRuntime::MSHeapThresholdState state;
    for (size_t sample = 10; sample <= 80; sample += 10) {
        (void)AcceptLiveSample(state, sample);
    }
    MapleRuntime::MSStableLiveDecision wrapped = AcceptLiveSample(state, 90);

    ExpectEqual(wrapped.historySize, MapleRuntime::kMSHeapThresholdHistoryLength,
                "wrapped history must remain at its fixed capacity");
    ExpectEqual(wrapped.medianStableLive, 55,
                "the ninth sample must evict the oldest of eight accepted samples");
}

void TestFloatingLiveIsExcludedFromStableHistory()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    constexpr size_t gib = 1024ULL * mib;
    MapleRuntime::MSHeapThresholdState state;
    (void)AcceptLiveSample(state, 1000 * mib);
    (void)AcceptLiveSample(state, 1200 * mib);
    (void)AcceptLiveSample(state, 1100 * mib);
    size_t nextHistoryIndex = state.nextHistoryIndex;

    MapleRuntime::MSStableLiveDecision decision = MapleRuntime::EvaluateMSStableLive(
        state, 1100 * mib, 1200 * mib, 15500 * mib, 13 * gib, false);

    ExpectTrue(decision.floatingLiveInflated, "case27 cycle must be classified as floating-live inflated");
    ExpectFalse(decision.acceptedSample, "floating-live sample must not enter stable history");
    ExpectFalse(decision.exceptionalBypass, "floating-live classification is not an exceptional bypass");
    ExpectEqual(decision.historySize, 3, "floating-live cycle must preserve history size");
    ExpectEqual(state.nextHistoryIndex, nextHistoryIndex,
                "floating-live cycle must not advance the history ring");
    ExpectEqual(decision.medianStableLive, 1100 * mib,
                "floating-live cycle must retain the accepted history median");
    ExpectEqual(decision.thresholdBase, 1100 * mib,
                "floating-live cycle must calculate from the accepted history median");
}

void TestFloatingLiveWithoutHistoryUsesMarkStartFallback()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    constexpr size_t gib = 1024ULL * mib;
    MapleRuntime::MSHeapThresholdState state;
    MapleRuntime::MSStableLiveDecision decision = MapleRuntime::EvaluateMSStableLive(
        state, 800 * mib, 1200 * mib, 15500 * mib, 13 * gib, false);

    ExpectTrue(decision.floatingLiveInflated, "case27 cycle must be classified as floating-live inflated");
    ExpectEqual(decision.thresholdBase, 1200 * mib,
                "empty-history floating cycle must use max(lastStableLive, markStartActive)");
    ExpectEqual(decision.medianStableLive, 0,
                "empty-history floating cycle must not invent a history median");
    ExpectEqual(decision.nextStableLive, 800 * mib,
                "inflated cycle must not overwrite the last stable live sample");
}

void TestExceptionalCycleBypassesAndPreservesStableHistory()
{
    MapleRuntime::MSHeapThresholdState state;
    (void)AcceptLiveSample(state, 100);
    (void)AcceptLiveSample(state, 300);
    size_t nextHistoryIndex = state.nextHistoryIndex;

    MapleRuntime::MSStableLiveDecision decision = MapleRuntime::EvaluateMSStableLive(
        state, 200, 100, 700, 550, true);

    ExpectTrue(decision.exceptionalBypass, "exceptional cycle must report policy bypass");
    ExpectFalse(decision.acceptedSample, "exceptional live sample must not enter normal history");
    ExpectEqual(decision.thresholdBase, 550, "exceptional cycle must use current bitmap live");
    ExpectEqual(decision.medianStableLive, 0,
                "exceptional cycle must not read normal history as its baseline");
    ExpectEqual(decision.nextStableLive, 200,
                "exceptional cycle must preserve the previous stable live statistic");
    ExpectEqual(decision.historySize, 2, "exceptional cycle must preserve history size");
    ExpectEqual(state.nextHistoryIndex, nextHistoryIndex,
                "exceptional cycle must not advance the history ring");
}

void TestThresholdDeadbandIsInclusive()
{
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(400, 360, 0.0, 0, 10000).threshold, 400,
                "lower ten-percent boundary must hold the old threshold");
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(400, 440, 0.0, 0, 10000).threshold, 400,
                "upper ten-percent boundary must hold the old threshold");
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(400, 359, 0.0, 0, 10000).threshold, 359,
                "a raw target below the deadband must begin decreasing");
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(400, 441, 0.0, 0, 10000).threshold, 441,
                "a raw target above the deadband must begin increasing");
}

void TestThresholdTransitionIsLimitedSymmetrically()
{
    MapleRuntime::MSHeapThresholdDecision rise =
        MapleRuntime::EvaluateMSHeapThreshold(400, 800, 0.0, 0, 10000);
    ExpectEqual(rise.threshold, 450, "large rise must be limited to twelve and a half percent");
    ExpectTrue(rise.riseLimited, "limited rise must be reported");
    ExpectTrue(rise.transitionLimited, "limited rise must report a limited transition");

    MapleRuntime::MSHeapThresholdDecision fall =
        MapleRuntime::EvaluateMSHeapThreshold(400, 100, 0.0, 0, 10000);
    ExpectEqual(fall.threshold, 350, "large fall must be limited to twelve and a half percent");
    ExpectTrue(fall.fallLimited, "limited fall must be reported");
    ExpectTrue(fall.transitionLimited, "limited fall must report a limited transition");
}

void TestThresholdBootstrapBypassesSmoothing()
{
    MapleRuntime::MSHeapThresholdDecision decision =
        MapleRuntime::EvaluateMSHeapThreshold(400, 800, 0.0, 0, 10000, true);
    ExpectEqual(decision.threshold, 800, "bootstrap or exceptional cycle must use the raw target");
    ExpectTrue(decision.transitionBypassed, "direct transition must report smoothing bypass");
    ExpectFalse(decision.transitionLimited, "bypassed transition must not report rate limiting");
}

void TestThresholdFloorAndCapRemainHardBounds()
{
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(20, 1, 0.0, 20, 1000).threshold, 20,
                "minimum threshold must remain effective");
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(900, 2000, 0.0, 20, 1000).threshold, 1000,
                "heap cap must remain effective");
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(2000, 500, 0.0, 20, 1000).threshold, 500,
                "an old threshold outside hard bounds must re-establish from the raw target");
}

void TestThresholdFloorUsesOnlyAbsoluteFloor()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    ExpectEqual(
        MapleRuntime::ComputeMSHeapThresholdFloor(
            20 * mib, 12ULL * 1024 * mib),
        20 * mib,
        "the mark threshold floor must not add a fixed concurrent-mark runway");
    ExpectEqual(
        MapleRuntime::ComputeMSHeapThresholdFloor(
            2ULL * 1024 * mib, 1024 * mib),
        1024 * mib,
        "the absolute floor must remain capped by the maximum heap");
}

void TestMarkRunwayUsesPreviousCompletedMarkAllocation()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSMarkRunwayDecision decision =
        MapleRuntime::ComputeMSMarkRunway(200 * mib, 100 * mib, 1000 * mib);

    ExpectEqual(decision.heapGoalGapBytes, 900 * mib,
                "runway gap must span previous trueLive to the heap goal");
    ExpectEqual(decision.estimatedRunwayBytes, 200 * mib,
                "runway estimate must use bytes allocated during the completed Mark");
    ExpectEqual(decision.runwayBytes, 200 * mib,
                "an estimate inside Go-style bounds must be preserved");
}

void TestMarkRunwayUsesGoStyleTriggerBounds()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSMarkRunwayDecision low =
        MapleRuntime::ComputeMSMarkRunway(0, 100 * mib, 1000 * mib);
    ExpectEqual(low.runwayBytes, 4 * mib,
                "an empty estimate must retain Go's minimum large-heap runway");

    MapleRuntime::MSMarkRunwayDecision high =
        MapleRuntime::ComputeMSMarkRunway(900 * mib, 100 * mib, 1000 * mib);
    ExpectEqual(high.runwayBytes, 270 * mib,
                "runway must not trigger earlier than seventy percent through the live-to-goal gap");
}

void TestMarkTriggerAddsRunwayWithoutOverflow()
{
    ExpectFalse(MapleRuntime::ShouldTriggerMSMark(699, 300, 1000),
                "swept bytes below goal minus runway must not trigger Mark");
    ExpectTrue(MapleRuntime::ShouldTriggerMSMark(700, 300, 1000),
               "swept bytes plus runway at the goal must trigger Mark");
    ExpectTrue(MapleRuntime::ShouldTriggerMSMark(static_cast<size_t>(-2), 8, static_cast<size_t>(-1)),
               "trigger arithmetic must remain correct near size_t overflow");
}

void TestScavengeThresholdTracksStableHeapInUse()
{
    MapleRuntime::MSScavengeThresholdState state;
    MapleRuntime::MSScavengeThresholdDecision decision =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 1000, 2000, 10000, 1.10, 1.0, 0);

    ExpectEqual(decision.scaledHeapInUseBytes, 1000,
                "a stable heap goal must preserve the post-GC in-use baseline");
    ExpectEqual(decision.retainedHeadroomBytes, 100,
                "the default Go-style multiplier must retain ten percent headroom");
    ExpectEqual(decision.candidateBytes, 1100,
                "the Go-style candidate must be post-GC in-use plus bounded headroom");
    ExpectEqual(decision.thresholdBytes, 2000,
                "the retained target must preserve runway to the next GC goal");
}

void TestScavengeThresholdRetainsRunwayToNextGC()
{
    MapleRuntime::MSScavengeThresholdState state;
    MapleRuntime::MSScavengeThresholdDecision decision =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 1000, 2000, 10000, 1.10, 1.0, 0);

    ExpectEqual(decision.thresholdBytes, 2000,
                "the retained target must not fall below the next GC heap goal");
}

void TestScavengeThresholdScalesNextHeapGoalIndependently()
{
    MapleRuntime::MSScavengeThresholdState state;
    MapleRuntime::MSScavengeThresholdDecision decision =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 1000, 2000, 10000, 1.10, 2.0, 0);

    ExpectEqual(decision.candidateBytes, 1100,
                "heap-goal scaling must not change the in-use candidate");
    ExpectEqual(decision.scaledHeapGoalBytes, 4000,
                "the next heap goal must use its independent multiplier");
    ExpectEqual(decision.thresholdBytes, 4000,
                "the retained target must preserve twice the next heap goal");
}

void TestScavengeThresholdDoesNotAmplifyHeapGoalRatio()
{
    MapleRuntime::MSScavengeThresholdState growingState;
    MapleRuntime::MSScavengeThresholdDecision growing =
        MapleRuntime::EvaluateMSScavengeThreshold(
            growingState, 1000, 4000, 10000, 1.10, 1.0, 0);
    ExpectEqual(growing.scaledHeapInUseBytes, 1000,
                "a changing heap goal must not amplify physical in-use bytes");
    ExpectEqual(growing.candidateBytes, 1100,
                "headroom must be applied to observed in-use bytes only");
    ExpectEqual(growing.thresholdBytes, 4000,
                "a growing heap must retain runway to its next GC goal");

    MapleRuntime::MSScavengeThresholdState shrinkingState;
    MapleRuntime::MSScavengeThresholdDecision shrinking =
        MapleRuntime::EvaluateMSScavengeThreshold(
            shrinkingState, 1000, 2000, 10000, 1.10, 1.0, 0);
    ExpectEqual(shrinking.scaledHeapInUseBytes, 1000,
                "a shrinking heap goal must preserve observed physical in-use bytes");
    ExpectEqual(shrinking.candidateBytes, 1100,
                "a shrinking goal must not scale the in-use estimate down");
    ExpectEqual(shrinking.thresholdBytes, 2000,
                "a shrinking heap must retain runway to its smaller next GC goal");
}

void TestScavengeThresholdBootstrapsWithUnitGoalRatio()
{
    MapleRuntime::MSScavengeThresholdState state;
    MapleRuntime::MSScavengeThresholdDecision decision =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 1000, 4000, 10000, 1.10, 1.0, 0);
    ExpectEqual(decision.scaledHeapInUseBytes, 1000,
                "the first GC must use a unit heap-goal ratio");
    ExpectEqual(decision.thresholdBytes, 4000,
                "bootstrap must retain runway to the first published heap goal");
}

void TestScavengeThresholdClampsMultiplierAndHeapCap()
{
    MapleRuntime::MSScavengeThresholdState noShrinkState;
    MapleRuntime::MSScavengeThresholdDecision noShrinkHeadroom =
        MapleRuntime::EvaluateMSScavengeThreshold(
            noShrinkState, 1000, 2000, 10000, 0.50, 1.0, 0);
    ExpectEqual(noShrinkHeadroom.candidateBytes, 1000,
                "a multiplier below one must not target less than the scaled in-use estimate");
    ExpectEqual(noShrinkHeadroom.thresholdBytes, 2000,
                "the next GC goal must remain the retained-target floor");

    MapleRuntime::MSScavengeThresholdState cappedState;
    MapleRuntime::MSScavengeThresholdDecision capped =
        MapleRuntime::EvaluateMSScavengeThreshold(
            cappedState, 900, 2000, 1000, 1.10, 1.0, 0);
    ExpectEqual(capped.candidateBytes, 990,
                "the uncapped candidate must remain visible for diagnostics");
    ExpectEqual(capped.thresholdBytes, 1000,
                "the retained target must respect the maximum heap cap");
}

void TestScavengeThresholdRisesImmediatelyAndFallsSlowly()
{
    MapleRuntime::MSScavengeThresholdState state;
    MapleRuntime::MSScavengeThresholdDecision initial =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 1000, 2000, 10000, 1.10, 1.0, 0);
    ExpectEqual(initial.thresholdBytes, 2000,
                "initial retained target must publish directly");

    MapleRuntime::MSScavengeThresholdDecision rise =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 1000, 4000, 10000, 1.10, 1.0, 0);
    ExpectEqual(rise.thresholdBytes, 4000,
                "a larger retained target must take effect immediately");

    for (size_t window = 1;
         window < MapleRuntime::kMSScavengeFallWindows; ++window) {
        MapleRuntime::MSScavengeThresholdDecision held =
            MapleRuntime::EvaluateMSScavengeThreshold(
                state, 1000, 2000, 10000, 1.10, 1.0, 0);
        ExpectEqual(held.thresholdBytes, 4000,
                    "a short-lived lower target must not trigger physical release");
    }
    MapleRuntime::MSScavengeThresholdDecision fall =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 1000, 2000, 10000, 1.10, 1.0, 0);
    ExpectEqual(fall.thresholdBytes, 3750,
                "an accepted fall must be limited to one sixteenth");
}

void TestScavengeColdReuseAddsDecayingReserve()
{
    MapleRuntime::MSScavengeThresholdState state;
    (void)MapleRuntime::EvaluateMSScavengeThreshold(
        state, 1000, 2000, 10000, 1.10, 1.0, 0);
    MapleRuntime::MSScavengeThresholdDecision reuse =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 1000, 2000, 10000, 1.10, 1.0, 300);
    ExpectEqual(reuse.coldReuseReserveBytes, 600,
                "cold reuse must retain twice the refaulted bytes");
    ExpectEqual(reuse.thresholdBytes, 2600,
                "cold reuse reserve must raise the retained target immediately");

    MapleRuntime::MSScavengeThresholdDecision decay =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 1000, 2000, 10000, 1.10, 1.0, 0);
    ExpectEqual(decay.coldReuseReserveBytes, 525,
                "an idle cycle must decay one eighth of the reserve");
    ExpectEqual(decay.thresholdBytes, 2600,
                "reserve decay must still obey delayed physical-target falls");
}

} // namespace

int main()
{
    TestStableLiveHistoryUsesAvailableMedian();
    TestStableLiveEvenMedianDoesNotOverflow();
    TestStableLiveHistoryEvictsOldestSampleAfterWrap();
    TestFloatingLiveIsExcludedFromStableHistory();
    TestFloatingLiveWithoutHistoryUsesMarkStartFallback();
    TestExceptionalCycleBypassesAndPreservesStableHistory();
    TestThresholdDeadbandIsInclusive();
    TestThresholdTransitionIsLimitedSymmetrically();
    TestThresholdBootstrapBypassesSmoothing();
    TestThresholdFloorAndCapRemainHardBounds();
    TestThresholdFloorUsesOnlyAbsoluteFloor();
    TestMarkRunwayUsesPreviousCompletedMarkAllocation();
    TestMarkRunwayUsesGoStyleTriggerBounds();
    TestMarkTriggerAddsRunwayWithoutOverflow();
    TestScavengeThresholdTracksStableHeapInUse();
    TestScavengeThresholdRetainsRunwayToNextGC();
    TestScavengeThresholdScalesNextHeapGoalIndependently();
    TestScavengeThresholdDoesNotAmplifyHeapGoalRatio();
    TestScavengeThresholdBootstrapsWithUnitGoalRatio();
    TestScavengeThresholdClampsMultiplierAndHeapCap();
    TestScavengeThresholdRisesImmediatelyAndFallsSlowly();
    TestScavengeColdReuseAddsDecayingReserve();
    return 0;
}
