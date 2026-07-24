#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

#define MRT_USE_MARK_SWEEP 1
#include "src/Heap/MAllocator/MSParallelSweepPolicy.h"
#include "src/Heap/MAllocator/MSSweepDebtPolicy.h"

namespace {

using MapleRuntime::BuildMSSweepCyclicClassOrder;
using MapleRuntime::BuildMSSweepPressureClassOrder;
using MapleRuntime::ComputeMSActiveSweepHelpers;
using MapleRuntime::ComputeMSMaxSweepHelpers;
using MapleRuntime::ComputeMSMediumSweepTargetHelpers;
using MapleRuntime::ComputeMSSweepSliceBudgetUs;
using MapleRuntime::ComputeMSSweepTargetHelpers;
using MapleRuntime::IsMSSweepQuiescent;
using MapleRuntime::MSSweepClassPressure;
using MapleRuntime::SweepPressureMode;

void ExpectEq(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n", message, expected, actual);
        std::abort();
    }
}

void TestHelperCaps()
{
    ExpectEq(ComputeMSMaxSweepHelpers(1, 0), 0, "one GC thread");
    ExpectEq(ComputeMSMaxSweepHelpers(8, 2), 2, "configured cap");
    ExpectEq(ComputeMSMaxSweepHelpers(3, 8), 2, "GC thread cap");
    ExpectEq(ComputeMSMaxSweepHelpers(4, 0), 3, "low-width automatic cap");
    ExpectEq(ComputeMSMaxSweepHelpers(16, 0), 8, "automatic sweep cap");
    ExpectEq(ComputeMSMaxSweepHelpers(16, 3), 3, "explicit sweep cap");
}

void TestMediumTargetIsProportionalToExcess()
{
    ExpectEq(ComputeMSMediumSweepTargetHelpers(60, 60, 100, 4), 1, "twenty percent excess");
    ExpectEq(ComputeMSMediumSweepTargetHelpers(90, 60, 100, 4), 2, "half-threshold excess");
    ExpectEq(ComputeMSMediumSweepTargetHelpers(99, 100, 100, 4), 4, "near-threshold excess");
    ExpectEq(ComputeMSMediumSweepTargetHelpers(100, 100, 100, 0), 0, "no available helpers");
    ExpectEq(ComputeMSMediumSweepTargetHelpers(std::numeric_limits<size_t>::max() - 10, 20,
                                               std::numeric_limits<size_t>::max(), 4),
             1, "overflow-safe excess");
}

void TestPressureSelectsTargetAndActualHelpers()
{
    ExpectEq(ComputeMSSweepTargetHelpers(SweepPressureMode::LOW, 60, 40, 100, 4), 0,
             "low pressure target");
    ExpectEq(ComputeMSSweepTargetHelpers(SweepPressureMode::MEDIUM, 90, 60, 100, 4), 2,
             "medium pressure target");
    ExpectEq(ComputeMSSweepTargetHelpers(SweepPressureMode::HIGH, 0, 101, 100, 4), 4,
             "high pressure target");

    ExpectEq(ComputeMSActiveSweepHelpers(1, 4, SweepPressureMode::MEDIUM, false), 2,
             "medium pressure grows one helper per round");
    ExpectEq(ComputeMSActiveSweepHelpers(4, 1, SweepPressureMode::MEDIUM, false), 3,
             "medium pressure drops one helper per round");
    ExpectEq(ComputeMSActiveSweepHelpers(3, 0, SweepPressureMode::LOW, false), 2,
             "low pressure retires one helper per round");
    ExpectEq(ComputeMSActiveSweepHelpers(1, 4, SweepPressureMode::HIGH, false), 4,
             "natural high pressure activates all helpers");
    ExpectEq(ComputeMSActiveSweepHelpers(0, 4, SweepPressureMode::HIGH, true), 4,
             "forced high pressure activates all helpers");
}

void TestSliceBudgetUsesPressureMultiplier()
{
    ExpectEq(ComputeMSSweepSliceBudgetUs(2000, SweepPressureMode::LOW), 2000,
             "low pressure slice");
    ExpectEq(ComputeMSSweepSliceBudgetUs(2000, SweepPressureMode::MEDIUM), 8000,
             "medium pressure slice");
    ExpectEq(ComputeMSSweepSliceBudgetUs(2000, SweepPressureMode::HIGH), 20000,
             "high pressure slice");
    ExpectEq(ComputeMSSweepSliceBudgetUs(std::numeric_limits<size_t>::max(), SweepPressureMode::HIGH),
             std::numeric_limits<size_t>::max(), "slice budget saturation");
}

void TestSweepQuiescenceIncludesEverySource()
{
    if (!IsMSSweepQuiescent(true, false, 0, 0, 0)) {
        std::abort();
    }
    if (IsMSSweepQuiescent(false, false, 0, 0, 0) ||
        IsMSSweepQuiescent(true, true, 0, 0, 0) ||
        IsMSSweepQuiescent(true, false, 1, 0, 0) ||
        IsMSSweepQuiescent(true, false, 0, 1, 0) ||
        IsMSSweepQuiescent(true, false, 0, 0, 1)) {
        std::abort();
    }
}

void TestCyclicClassOrderStartsAtPreferredAndWraps()
{
    auto order = BuildMSSweepCyclicClassOrder<4>(2);
    ExpectEq(order[0], 2, "cyclic preferred class");
    ExpectEq(order[1], 3, "cyclic class after preferred");
    ExpectEq(order[2], 0, "cyclic wrap to zero");
    ExpectEq(order[3], 1, "cyclic final class");

    auto invalidOrder = BuildMSSweepCyclicClassOrder<4>(4);
    for (size_t cls = 0; cls < invalidOrder.size(); ++cls) {
        ExpectEq(invalidOrder[cls], cls, "invalid preferred class falls back to zero");
    }
}

void TestPressureClassOrderUsesImmutableSnapshot()
{
    std::array<MSSweepClassPressure, 4> pressure {{
        {5, 0},
        {9, 3},
        {1, 1},
        {1, 4},
    }};

    auto order = BuildMSSweepPressureClassOrder(pressure, 4);
    ExpectEq(order[0], 3, "larger backlog breaks equal-ready tie");
    ExpectEq(order[1], 2, "lower ready supply precedes class with more supply");
    ExpectEq(order[2], 1, "remaining unswept class precedes empty class");
    ExpectEq(order[3], 0, "class without unswept work is last");

    auto preferredOrder = BuildMSSweepPressureClassOrder(pressure, 1);
    ExpectEq(preferredOrder[0], 1, "preferred class overrides pressure rank");
    ExpectEq(preferredOrder[1], 3, "pressure rank remains after preferred class");
    ExpectEq(preferredOrder[2], 2, "lower ready class remains ahead");
    ExpectEq(preferredOrder[3], 0, "empty class remains last");
}

} // namespace

int main()
{
    TestHelperCaps();
    TestMediumTargetIsProportionalToExcess();
    TestPressureSelectsTargetAndActualHelpers();
    TestSliceBudgetUsesPressureMultiplier();
    TestSweepQuiescenceIncludesEverySource();
    TestCyclicClassOrderStartsAtPreferredAndWraps();
    TestPressureClassOrderUsesImmutableSnapshot();
    return 0;
}
