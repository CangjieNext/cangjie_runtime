#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSMarkWorkerPolicy.h"

namespace {

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

void TestThreadCountResolution()
{
    ExpectEq(ResolveMSGCThreadCapacity(1, 128), 128, "pool capacity follows machine logical cores");
    ExpectEq(ResolveMSGCThreadCapacity(160, 8), 8, "logical cores define MSGC capacity");
    ExpectEq(ResolveMSGCThreadCapacity(8, 128), 128, "processor setting does not shrink pool capacity");
    ExpectEq(ResolveMSGCThreadCapacity(4, 0), 4, "processor count is used only when hardware count is unavailable");
    ExpectEq(ResolveMSMarkTotalThreads(1, 128, 0), 1, "one processor keeps one total GC thread");
    ExpectEq(ResolveMSMarkTotalThreads(4, 128, 0), 2, "default GC threads use half of P");
    ExpectEq(ResolveMSMarkTotalThreads(5, 128, 0), 3, "odd processor counts round the default half up");
    ExpectEq(ResolveMSMarkTotalThreads(8, 128, 0), 4, "default GC threads use half of P");
    ExpectEq(ResolveMSMarkTotalThreads(32, 128, 0), 16, "default GC threads use half of P");
    ExpectEq(ResolveMSMarkTotalThreads(64, 128, 0), 32, "default GC threads use half of P");
    ExpectEq(ResolveMSMarkTotalThreads(128, 128, 0), 64, "default GC threads use half of machine cores");
    ExpectEq(ResolveMSMarkTotalThreads(256, 256, 0), 128, "default GC threads use half of machine cores");
    ExpectEq(ResolveMSMarkTotalThreads(160, 8, 0), 4, "machine half caps a wider processor setting");
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

} // namespace

int main()
{
    TestThreadCountResolution();
    TestPreEnumResetHelperSteps();
    return 0;
}
