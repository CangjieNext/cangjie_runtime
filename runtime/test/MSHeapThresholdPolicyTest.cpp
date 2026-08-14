#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSHeapThresholdPolicy.h"
#include "src/Heap/MCollector/MSMemoryLimitPolicy.h"
#include "src/Heap/MCollector/MSScavengeWatermarkPolicy.h"

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
    ExpectEqual(MapleRuntime::ComputeMSScavengeThreshold(2000, 10000, 1.20),
                2400,
                "Scavenge growth must scale only the GC goal");
}

void TestScavengeThresholdClampsGrowthAndHeapCap()
{
    ExpectEqual(MapleRuntime::ComputeMSScavengeThreshold(2000, 10000, 0.50),
                2000,
                "Scavenge growth below one must not shrink the base");

    ExpectEqual(MapleRuntime::ComputeMSScavengeThreshold(2000, 2200, 1.20),
                2200,
                "Scavenge threshold must respect its supplied heap/soft-limit cap");
}

} // namespace

int main()
{
    TestThresholdDeadbandIsInclusive();
    TestThresholdTransitionIsLimitedSymmetrically();
    TestThresholdBootstrapBypassesSmoothing();
    TestThresholdFloorAndCapRemainHardBounds();
    TestThresholdFloorUsesOnlyAbsoluteFloor();
    TestMarkTriggerSubtractsOutsideSupplyButPreservesSoftLimitOOM();
    TestScavengeThresholdUsesGCGoalOnly();
    TestScavengeThresholdClampsGrowthAndHeapCap();
    return 0;
}
