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

} // namespace

int main()
{
    TestDecreaseFollowsRawTargetImmediately();
    TestSmallIncreaseReachesRawTarget();
    TestLargeIncreaseIsRateLimited();
    TestLiveSafetyFloorOverridesRiseLimit();
    TestFloorAndCapStillApply();
    return 0;
}
