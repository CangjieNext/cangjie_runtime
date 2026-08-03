#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSMarkWorkerPolicy.h"

namespace {

using MapleRuntime::MSMarkWorkerPolicy;
using MapleRuntime::ComputeMSEnumRootsHelperTarget;
using MapleRuntime::ComputeMSPostMarkStatsHelperTarget;
using MapleRuntime::ComputeMSPreEnumResetHelperTarget;
using MapleRuntime::ResolveMSGCThreadCapacity;
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
    ExpectEq(ResolveMSGCThreadCapacity(1, 128), 128, "pool capacity follows machine logical cores");
    ExpectEq(ResolveMSGCThreadCapacity(160, 8), 8, "logical cores define MSGC capacity");
    ExpectEq(ResolveMSGCThreadCapacity(8, 128), 128, "processor setting does not shrink pool capacity");
    ExpectEq(ResolveMSGCThreadCapacity(4, 0), 4, "processor count is used only when hardware count is unavailable");
    ExpectEq(ResolveMSMarkTotalThreads(1, 128, 0), 128, "default mark threads use machine capacity");
    ExpectEq(ResolveMSMarkTotalThreads(4, 128, 0), 128, "default mark threads use machine capacity");
    ExpectEq(ResolveMSMarkTotalThreads(8, 128, 0), 128, "default mark threads use machine capacity");
    ExpectEq(ResolveMSMarkTotalThreads(32, 128, 0), 128, "default mark threads use machine capacity");
    ExpectEq(ResolveMSMarkTotalThreads(64, 128, 0), 128, "default mark threads use machine capacity");
    ExpectEq(ResolveMSMarkTotalThreads(128, 128, 0), 128, "default mark threads use machine capacity");
    ExpectEq(ResolveMSMarkTotalThreads(256, 256, 0), 256, "default mark follows machine capacity");
    ExpectEq(ResolveMSMarkTotalThreads(160, 8, 0), 8, "160P on 8 logical CPUs");
    ExpectEq(ResolveMSMarkTotalThreads(8, 128, 4), 4,
             "explicit mark threads should enable four total GC threads");
    ExpectEq(ResolveMSMarkTotalThreads(160, 8, 12), 8,
             "explicit mark threads should be limited by available CPUs only");
    ExpectEq(ResolveMSMarkTotalThreads(2, 128, 8), 8,
             "explicit mark threads should not be limited by processor setting");
    ExpectEq(ResolveMSMarkTotalThreads(8, 128, 1), 1,
             "explicit single-thread mark should remain supported");
}

void TestPreEnumResetHelperSteps()
{
    constexpr size_t kMiB = 1024 * 1024;
    ExpectEq(ComputeMSPreEnumResetHelperTarget(kMiB - 1, 7), 0,
             "sub-1MiB reset stays serial");
    ExpectEq(ComputeMSPreEnumResetHelperTarget(kMiB, 7), 1,
             "1MiB reset starts one helper");
    ExpectEq(ComputeMSPreEnumResetHelperTarget(2 * kMiB - 1, 7), 1,
             "below 2MiB keeps one helper");
    ExpectEq(ComputeMSPreEnumResetHelperTarget(2 * kMiB, 7), 2,
             "2MiB reset starts two helpers");
    ExpectEq(ComputeMSPreEnumResetHelperTarget(4 * kMiB, 7), 3,
             "4MiB reset starts three helpers");
    ExpectEq(ComputeMSPreEnumResetHelperTarget(64 * kMiB, 3), 3,
             "reset helpers are capped by runtime capacity");
}

void TestEnumRootsHelperStepsFromInitialMutatorCount()
{
    ExpectEq(ComputeMSEnumRootsHelperTarget(0, 7), 0,
             "empty EnumRoots stays serial");
    ExpectEq(ComputeMSEnumRootsHelperTarget(999, 7), 0,
             "sub-1000 initial mutators stay serial");
    ExpectEq(ComputeMSEnumRootsHelperTarget(1000, 7), 1,
             "1000 initial mutators start one helper");
    ExpectEq(ComputeMSEnumRootsHelperTarget(1999, 7), 1,
             "below 2000 keeps one helper");
    ExpectEq(ComputeMSEnumRootsHelperTarget(2000, 7), 2,
             "2000 initial mutators start two helpers");
    ExpectEq(ComputeMSEnumRootsHelperTarget(3999, 7), 2,
             "below 4000 keeps two helpers");
    ExpectEq(ComputeMSEnumRootsHelperTarget(4000, 7), 3,
             "4000 initial mutators start three helpers");
    ExpectEq(ComputeMSEnumRootsHelperTarget(7999, 7), 3,
             "below 8000 keeps three helpers");
    ExpectEq(ComputeMSEnumRootsHelperTarget(8000, 3), 3,
             "EnumRoots helpers are capped by GCThreadPool capacity");
}

void TestPostMarkStatsHelpersAreReservedForLargeBitmapSets()
{
    constexpr size_t kMiB = 1024 * 1024;
    ExpectEq(ComputeMSPostMarkStatsHelperTarget(2 * kMiB - 1, 7), 0,
             "small bitmap statistics stay serial");
    ExpectEq(ComputeMSPostMarkStatsHelperTarget(2 * kMiB, 7), 1,
             "2MiB bitmap statistics start one helper");
    ExpectEq(ComputeMSPostMarkStatsHelperTarget(4 * kMiB, 7), 2,
             "4MiB bitmap statistics start two helpers");
    ExpectEq(ComputeMSPostMarkStatsHelperTarget(64 * kMiB, 7), 2,
             "bitmap statistics cap helpers to protect mutator bandwidth");
    ExpectEq(ComputeMSPostMarkStatsHelperTarget(64 * kMiB, 1), 1,
             "bitmap statistics respect pool capacity");
    ExpectEq(ComputeMSPostMarkStatsHelperTarget(64 * kMiB, 0), 0,
             "bitmap statistics stay serial without helpers");
}

} // namespace

int main()
{
    TestBacklogMapsDirectlyToHelperWidth();
    TestHelpersStayActiveUntilCycleQuiesces();
    TestThreadCountResolution();
    TestPreEnumResetHelperSteps();
    TestEnumRootsHelperStepsFromInitialMutatorCount();
    TestPostMarkStatsHelpersAreReservedForLargeBitmapSets();
    return 0;
}
