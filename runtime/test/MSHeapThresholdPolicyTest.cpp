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

void TestScavengeThresholdKeepsRecentBurstAsResidentReserve()
{
    constexpr size_t gib = 1024ULL * 1024ULL * 1024ULL;
    constexpr size_t segment = 64ULL * 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeThresholdState state;
    MapleRuntime::MSScavengeThresholdDecision decision =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 2 * gib, 14 * gib, 0, 16 * gib, 0, segment);

    ExpectEqual(decision.recentPeakInUseBytes, 14 * gib,
                "recent peak must include the current mark-end in-use sample");
    ExpectEqual(decision.recentBurstBytes, 12 * gib,
                "recent burst must use positive in-use growth during mark");
    ExpectEqual(decision.residentReserveBytes, 12 * gib,
                "recent burst must establish the resident reserve floor");
    ExpectEqual(decision.candidateBytes, 16 * gib,
                "recent peak plus resident reserve must saturate at the maximum heap");
    ExpectEqual(decision.thresholdBytes, 16 * gib,
                "a higher scavenge threshold must take effect immediately");
}

void TestColdReuseRaisesResidentReserveImmediately()
{
    constexpr size_t segment = 100;
    MapleRuntime::MSScavengeThresholdState state;
    MapleRuntime::MSScavengeThresholdDecision initial =
        MapleRuntime::EvaluateMSScavengeThreshold(state, 200, 200, 0, 1000, 0, segment);
    ExpectEqual(initial.residentReserveBytes, 0,
                "a stable first sample must not invent resident reserve");

    MapleRuntime::MSScavengeThresholdDecision reused =
        MapleRuntime::EvaluateMSScavengeThreshold(state, 200, 200, 75, 1000, 200, segment);
    ExpectEqual(reused.coldReuseBytes, 75,
                "decision must expose this GC cycle's cold reuse");
    ExpectEqual(reused.residentReserveBytes, 75,
                "cold reuse must immediately add equivalent resident reserve");
    ExpectEqual(reused.thresholdBytes, 275,
                "threshold must be recent peak plus resident reserve");
}

void TestResidentReserveDecaysOnlyAfterThirtyTwoQuietCycles()
{
    constexpr size_t segment = 100;
    MapleRuntime::MSScavengeThresholdState state;
    (void)MapleRuntime::EvaluateMSScavengeThreshold(state, 200, 200, 64, 1000, 0, segment);

    for (size_t i = 1; i < 32; ++i) {
        MapleRuntime::MSScavengeThresholdDecision quiet =
            MapleRuntime::EvaluateMSScavengeThreshold(state, 200, 200, 0, 1000, 264, segment);
        ExpectEqual(quiet.residentReserveBytes, 64,
                    "resident reserve must remain stable before thirty-two quiet cycles");
        ExpectFalse(quiet.decreased, "early quiet cycles must not decrease reserve");
    }

    MapleRuntime::MSScavengeThresholdDecision decay =
        MapleRuntime::EvaluateMSScavengeThreshold(state, 200, 200, 0, 1000, 264, segment);
    ExpectEqual(decay.residentReserveBytes, 63,
                "reserve must decay by ceil((reserve-floor)/64)");
    ExpectTrue(decay.decreased, "the thirty-second quiet cycle must report reserve decay");
}

void TestScavengePeakRemainsInSixtyFourCycleHistory()
{
    constexpr size_t segment = 100;
    MapleRuntime::MSScavengeThresholdState state;
    (void)MapleRuntime::EvaluateMSScavengeThreshold(state, 900, 900, 0, 10000, 0, segment);

    for (size_t i = 1; i < 64; ++i) {
        MapleRuntime::MSScavengeThresholdDecision decision =
            MapleRuntime::EvaluateMSScavengeThreshold(state, 600, 600, 0, 10000, 900, segment);
        ExpectEqual(decision.recentPeakInUseBytes, 900,
                    "the high in-use sample must remain protected for sixty-four cycles");
    }

    MapleRuntime::MSScavengeThresholdDecision expired =
        MapleRuntime::EvaluateMSScavengeThreshold(state, 600, 600, 0, 10000, 900, segment);
    ExpectEqual(expired.recentPeakInUseBytes, 600,
                "the high sample must expire after sixty-four newer samples");
}

void TestScavengeThresholdRiseIsLimitedToOneSegment()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeThresholdState state;
    MapleRuntime::MSScavengeThresholdDecision decision =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 256 * mib, 512 * mib, 0, 2ULL * 1024 * mib, 256 * mib, 64 * mib);

    ExpectEqual(decision.candidateBytes, 768 * mib,
                "candidate must retain the recent peak and burst reserve");
    ExpectEqual(decision.thresholdBytes, 320 * mib,
                "a large rise must advance by only one segment");
    ExpectTrue(decision.thresholdChangeLimited,
               "a segment-limited rise must be reported");
}

void TestScavengeThresholdDecreaseIsLimitedToOneSegment()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeThresholdState state;
    MapleRuntime::MSScavengeThresholdDecision decision =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 128 * mib, 128 * mib, 0, 2ULL * 1024 * mib, 512 * mib, 64 * mib);

    ExpectEqual(decision.candidateBytes, 128 * mib,
                "quiet candidate must reflect current history");
    ExpectEqual(decision.thresholdBytes, 448 * mib,
                "a large decrease must retreat by only one segment");
    ExpectTrue(decision.thresholdChangeLimited,
               "a segment-limited decrease must be reported");
}

void TestScavengeThresholdReachesCandidateWithinOneSegment()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeThresholdState state;
    MapleRuntime::MSScavengeThresholdDecision decision =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 280 * mib, 280 * mib, 0, 2ULL * 1024 * mib, 256 * mib, 64 * mib);

    ExpectEqual(decision.thresholdBytes, 280 * mib,
                "a candidate within one segment must take effect");
    ExpectFalse(decision.thresholdChangeLimited,
                "an in-range candidate must not report rate limiting");
}

void TestScavengeThresholdBootstrapsWithoutSegmentLimit()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSScavengeThresholdState state;
    MapleRuntime::MSScavengeThresholdDecision decision =
        MapleRuntime::EvaluateMSScavengeThreshold(
            state, 512 * mib, 512 * mib, 0, 2ULL * 1024 * mib, 0, 64 * mib);

    ExpectEqual(decision.thresholdBytes, 512 * mib,
                "zero sentinel must bootstrap directly to the first candidate");
    ExpectFalse(decision.thresholdChangeLimited,
                "initialization must not report rate limiting");
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
    TestScavengeThresholdKeepsRecentBurstAsResidentReserve();
    TestColdReuseRaisesResidentReserveImmediately();
    TestResidentReserveDecaysOnlyAfterThirtyTwoQuietCycles();
    TestScavengePeakRemainsInSixtyFourCycleHistory();
    TestScavengeThresholdRiseIsLimitedToOneSegment();
    TestScavengeThresholdDecreaseIsLimitedToOneSegment();
    TestScavengeThresholdReachesCandidateWithinOneSegment();
    TestScavengeThresholdBootstrapsWithoutSegmentLimit();
    return 0;
}
