#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSHeapThresholdPolicy.h"
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

void TestGCThresholdBaseUsesMarkedLiveOnly()
{
    MapleRuntime::MSGCThresholdBaseDecision decision =
        MapleRuntime::ComputeMSGCThresholdBase(100);

    ExpectEqual(decision.markedLiveBytes, 100,
                "GC base must retain the frozen marked-live sample");
    ExpectEqual(decision.gcBaseBytes, 100,
                "GC base must not add TLS or outside allocation supply");
}

void TestGCThresholdBasePreservesLargeMarkedLive()
{
    constexpr size_t maximum = static_cast<size_t>(-1);
    MapleRuntime::MSGCThresholdBaseDecision decision =
        MapleRuntime::ComputeMSGCThresholdBase(maximum - 1);

    ExpectEqual(decision.gcBaseBytes, maximum - 1,
                "GC base must be exactly the marked-live value");
}

void TestMarkTriggerUsesSweptBytesWithoutRunway()
{
    ExpectFalse(MapleRuntime::ShouldTriggerMSMark(999, 1000),
                "swept bytes below the GC threshold must not trigger Mark");
    ExpectTrue(MapleRuntime::ShouldTriggerMSMark(1000, 1000),
               "swept bytes equal to the GC threshold must trigger Mark");
    ExpectTrue(MapleRuntime::ShouldTriggerMSMark(1001, 1000),
               "swept bytes above the GC threshold must trigger Mark");
}

void TestMarkTriggerSubtractsOutsideSupplyButPreservesSoftLimitOOM()
{
    size_t effectiveSweptBytes = MapleRuntime::SaturatingSubtractMSBytes(1200, 300);
    ExpectEqual(effectiveSweptBytes, 900,
                "HEU pressure must exclude the previous mark's outside supply");
    ExpectEqual(MapleRuntime::SaturatingSubtractMSBytes(300, 1200), 0,
                "outside-supply correction must saturate at zero");
    ExpectTrue(
        MapleRuntime::ResolveMSMarkTrigger(10000, effectiveSweptBytes, 1000, 10000) ==
            MapleRuntime::MSMarkTrigger::OOM,
        "soft-limit OOM must take precedence over outside-adjusted HEU pressure");
    ExpectTrue(
        MapleRuntime::ResolveMSMarkTrigger(9999, 1000, 1000, 10000) ==
            MapleRuntime::MSMarkTrigger::HEU,
        "HEU must trigger when outside-adjusted swept bytes reach the GC goal");
}

void TestScavengeThresholdUsesGCGoalOnly()
{
    MapleRuntime::MSScavengeThresholdDecision decision =
        MapleRuntime::EvaluateMSScavengeThreshold(
            2000, 10000, 1.20);

    ExpectEqual(decision.baseBytes, 2000,
                "Scavenge base must be the completed GC goal only");
    ExpectEqual(decision.thresholdBytes, 2399,
                "Scavenge growth must scale only the GC goal with the policy's truncating semantics");
}

void TestScavengeThresholdClampsGrowthAndHeapCap()
{
    MapleRuntime::MSScavengeThresholdDecision boundedGrowth =
        MapleRuntime::EvaluateMSScavengeThreshold(
            2000, 10000, 0.50);
    ExpectEqual(boundedGrowth.thresholdBytes, 2000,
                "Scavenge growth below one must not shrink the base");

    MapleRuntime::MSScavengeThresholdDecision capped =
        MapleRuntime::EvaluateMSScavengeThreshold(
            2000, 2200, 1.20);
    ExpectEqual(capped.thresholdBytes, 2200,
                "Scavenge threshold must respect its supplied heap/soft-limit cap");
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
    TestGCThresholdBaseUsesMarkedLiveOnly();
    TestGCThresholdBasePreservesLargeMarkedLive();
    TestMarkTriggerUsesSweptBytesWithoutRunway();
    TestMarkTriggerSubtractsOutsideSupplyButPreservesSoftLimitOOM();
    TestScavengeThresholdUsesGCGoalOnly();
    TestScavengeThresholdClampsGrowthAndHeapCap();
    return 0;
}
