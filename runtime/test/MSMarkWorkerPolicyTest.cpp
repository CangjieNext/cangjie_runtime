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

void TestBacklogMapsDirectlyToHelperWidth()
{
    MSMarkWorkerPolicy policy(15);
    ExpectEq(policy.Update(0), 1, "parallel mark should start one helper");
    ExpectEq(policy.Update(8), 1, "eight buffers should need one helper");
    ExpectEq(policy.Update(9), 2, "ninth buffer should activate second helper");
    ExpectEq(policy.Update(64), 8, "backlog should map proportionally");
    ExpectEq(policy.Update(120), 15, "backlog should reach helper cap");
}

void TestHelpersStayActiveUntilCycleQuiesces()
{
    MSMarkWorkerPolicy policy(15);
    (void)policy.Update(120);
    for (size_t i = 0; i < 64; ++i) {
        ExpectEq(policy.Update(0), 15, "active helpers must not scale down within a mark cycle");
    }
}

void TestThreadCountResolution()
{
    ExpectEq(ResolveMSMarkTotalThreads(1, 128, 0), 1, "1P default mark threads");
    ExpectEq(ResolveMSMarkTotalThreads(4, 128, 0), 2, "4P default mark threads");
    ExpectEq(ResolveMSMarkTotalThreads(8, 128, 0), 4, "8P default mark threads");
    ExpectEq(ResolveMSMarkTotalThreads(32, 128, 0), 4, "32P default mark threads");
    ExpectEq(ResolveMSMarkTotalThreads(64, 128, 0), 8, "64P default mark threads");
    ExpectEq(ResolveMSMarkTotalThreads(128, 128, 0), 16, "128P default mark threads");
    ExpectEq(ResolveMSMarkTotalThreads(256, 256, 0), 16, "default mark thread cap");
    ExpectEq(ResolveMSMarkTotalThreads(160, 8, 0), 4, "160P on 8 logical CPUs");
    ExpectEq(ResolveMSMarkTotalThreads(160, 8, 12), 12, "explicit mark thread override");
    ExpectEq(ResolveMSMarkTotalThreads(2, 128, 8), 2,
             "explicit mark threads should not exceed processors");
}

} // namespace

int main()
{
    TestBacklogMapsDirectlyToHelperWidth();
    TestHelpersStayActiveUntilCycleQuiesces();
    TestThreadCountResolution();
    return 0;
}
