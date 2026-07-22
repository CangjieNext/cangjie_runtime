#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSMarkWorkerPolicy.h"

namespace {

using MapleRuntime::MSMarkWorkerPolicy;
using MapleRuntime::ResolveMSMarkTotalThreads;

void ExpectEq(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n", message, expected, actual);
        std::abort();
    }
}

void TestGradualScaleUp()
{
    MSMarkWorkerPolicy policy(3);
    ExpectEq(policy.Update(9), 1, "backlog should activate first helper");
    ExpectEq(policy.Update(17), 2, "larger backlog should activate second helper");
    ExpectEq(policy.Update(25), 3, "larger backlog should activate third helper");
}

void TestEmergencyScaleUp()
{
    MSMarkWorkerPolicy policy(3);
    ExpectEq(policy.Update(64), 3, "emergency backlog should activate all helpers");
}

void TestHelpersStayActiveUntilCycleQuiesces()
{
    MSMarkWorkerPolicy policy(3);
    (void)policy.Update(64);
    for (size_t i = 0; i < 64; ++i) {
        ExpectEq(policy.Update(0), 3, "active helpers must not scale down within a mark cycle");
    }
}

void TestTailModePinsAllHelpers()
{
    MSMarkWorkerPolicy policy(3);
    ExpectEq(policy.Update(9), 1, "backlog should activate one helper before tail mode");

    ExpectEq(policy.EnterTail(), 3, "tail mode should activate every helper");

    for (size_t i = 0; i < 32; ++i) {
        ExpectEq(policy.Update(0), 3, "tail mode must prevent helper scale-down");
    }
}

void TestThreadCountResolution()
{
    ExpectEq(ResolveMSMarkTotalThreads(8, 0), 4, "8P default mark threads");
    ExpectEq(ResolveMSMarkTotalThreads(4, 0), 2, "4P default mark threads");
    ExpectEq(ResolveMSMarkTotalThreads(1, 0), 1, "1P default mark threads");
    ExpectEq(ResolveMSMarkTotalThreads(8, 3), 3, "explicit mark threads");
    ExpectEq(ResolveMSMarkTotalThreads(2, 8), 2, "explicit mark threads should not exceed processors");
}

} // namespace

int main()
{
    TestGradualScaleUp();
    TestEmergencyScaleUp();
    TestHelpersStayActiveUntilCycleQuiesces();
    TestTailModePinsAllHelpers();
    TestThreadCountResolution();
    return 0;
}
