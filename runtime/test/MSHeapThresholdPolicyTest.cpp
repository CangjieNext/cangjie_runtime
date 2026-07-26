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

void TestDecreaseFollowsRawTargetImmediately()
{
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(400, 150, 1.0, 0, 10000).threshold, 300,
                "threshold decrease must not be smoothed");
}

void TestSmallIncreaseReachesRawTarget()
{
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(320, 170, 1.0, 0, 10000).threshold, 340,
                "an increase below the rate limit should reach its raw target");
}

void TestLargeIncreaseIsRateLimited()
{
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(320, 250, 1.0, 0, 10000).threshold, 360,
                "a large increase should be limited to twelve and a half percent");
}

void TestLiveSafetyFloorOverridesRiseLimit()
{
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(320, 500, 1.0, 0, 10000).threshold, 625,
                "threshold must retain twenty-five percent headroom above current live bytes");
}

void TestFloorAndCapStillApply()
{
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(20, 1, 1.0, 20, 1000).threshold, 20,
                "minimum threshold should remain effective");
    ExpectEqual(MapleRuntime::EvaluateMSHeapThreshold(900, 1000, 1.0, 20, 1000).threshold, 1000,
                "heap cap should override smoothing and safety headroom");
}

void TestCase27FloatingLiveUsesStableBase()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    constexpr size_t gib = 1024ULL * mib;
    MapleRuntime::MSStableLiveDecision decision = MapleRuntime::EvaluateMSStableLive(
        800 * mib, 1200 * mib, 15500 * mib, 13 * gib);

    ExpectTrue(decision.floatingLiveInflated, "case27 cycle must be classified as floating-live inflated");
    ExpectEqual(decision.thresholdBase, 1200 * mib,
                "inflated cycle must use max(lastStableLive, markStartActive)");
    ExpectEqual(decision.nextStableLive, 800 * mib,
                "inflated cycle must not overwrite the last stable live sample");
}

void TestFloatingLiveDoesNotDependOnMaximumHeapQuarter()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    MapleRuntime::MSStableLiveDecision decision = MapleRuntime::EvaluateMSStableLive(
        80 * mib, 100 * mib, 700 * mib, 550 * mib);

    ExpectTrue(decision.floatingLiveInflated,
               "relative floating-live growth must not depend on consuming one quarter of max heap");
    ExpectEqual(decision.thresholdBase, 100 * mib,
                "relative floating-live growth must use the mark-start stable base");
    ExpectEqual(decision.nextStableLive, 80 * mib,
                "relative floating-live growth must preserve the previous stable sample");
}

void TestNormalLiveGrowthUpdatesStableBase()
{
    constexpr size_t mib = 1024ULL * 1024ULL;
    constexpr size_t gib = 1024ULL * mib;
    MapleRuntime::MSStableLiveDecision decision = MapleRuntime::EvaluateMSStableLive(
        800 * mib, 1 * gib, 2 * gib, 1500 * mib);

    ExpectFalse(decision.floatingLiveInflated, "ordinary live growth must not be classified as inflated");
    ExpectEqual(decision.thresholdBase, 1500 * mib, "ordinary cycle must use current bitmap live");
    ExpectEqual(decision.nextStableLive, 1500 * mib, "ordinary cycle must refresh stable live");
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
    TestDecreaseFollowsRawTargetImmediately();
    TestSmallIncreaseReachesRawTarget();
    TestLargeIncreaseIsRateLimited();
    TestLiveSafetyFloorOverridesRiseLimit();
    TestFloorAndCapStillApply();
    TestCase27FloatingLiveUsesStableBase();
    TestFloatingLiveDoesNotDependOnMaximumHeapQuarter();
    TestNormalLiveGrowthUpdatesStableBase();
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
