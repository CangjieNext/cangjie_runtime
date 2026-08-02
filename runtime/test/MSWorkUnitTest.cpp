#include <cstdio>
#include <cstdlib>

#define MRT_USE_MARK_SWEEP 1

#include "src/Heap/MCollector/MSWorkUnit.h"

namespace {

using namespace MapleRuntime;

static_assert(sizeof(MSWorkUnitCounter) == sizeof(uint64_t),
              "WorkUnit counter must use one weighted total");

void Expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void TestFixedIntegerCost()
{
    MSWorkUnitCounter work;
    work.AddResolvedRefs(5);
    work.AddMarkWordRMW(2);
    work.AddScanWords(3);
    Expect(work.Total() == 24, "WorkUnit fixed cost changed");
    work.Reset();
    Expect(work.Total() == 0, "WorkUnit reset did not clear weighted total");
}

void TestFullCycleEwma()
{
    MSWorkUnitPacer pacer;
    uint64_t oldDensity = pacer.GetWorkDensityQ16();
    uint64_t oldThroughput = pacer.GetThroughputQ16();
    pacer.UpdateFullCycle(1024, 32768, 16);
    Expect(
        pacer.GetWorkDensityQ16() ==
            UpdateMSWUEwmaOneEighth(
                oldDensity, MSWUQ16Divide(1024, 32768)),
        "density EWMA mismatch");
    Expect(
        pacer.GetThroughputQ16() ==
            UpdateMSWUEwmaOneEighth(
                oldThroughput, MSWUQ16Divide(1024, 16)),
        "throughput EWMA mismatch");
}

void TestAssistRatio()
{
    Expect(
        ComputeMSAssistRatioQ16(
            MS_WU_Q16_ONE, MSMarkPressure::NORMAL, 1) ==
            MS_WU_Q16_ONE / 4,
        "normal assist ratio mismatch");
    Expect(
        ComputeMSAssistRatioQ16(
            MS_WU_Q16_ONE, MSMarkPressure::CRITICAL, 1) ==
            MS_WU_Q16_ONE,
        "critical assist ratio mismatch");
}

void TestDynamicAssistRatioTracksHelperOutstandingLevels()
{
    constexpr uint64_t baseRatio = 100;
    constexpr size_t maxMultiplier = 4;
    Expect(
        ComputeMSMarkDynamicAssistRatioQ16(
            baseRatio, 0, maxMultiplier, MSMarkPressure::NORMAL) == baseRatio,
        "zero outstanding must preserve the cycle-start assist ratio floor");
    Expect(
        ComputeMSMarkDynamicAssistRatioQ16(
            baseRatio, 8, maxMultiplier, MSMarkPressure::NORMAL) == baseRatio,
        "first outstanding level must use the base assist ratio");
    Expect(
        ComputeMSMarkDynamicAssistRatioQ16(
            baseRatio, 9, maxMultiplier, MSMarkPressure::NORMAL) == 2 * baseRatio,
        "second outstanding level must double the assist ratio");
    Expect(
        ComputeMSMarkDynamicAssistRatioQ16(
            baseRatio, 17, maxMultiplier, MSMarkPressure::NORMAL) == 3 * baseRatio,
        "third outstanding level must triple the assist ratio");
    Expect(
        ComputeMSMarkDynamicAssistRatioQ16(
            baseRatio, 25, maxMultiplier, MSMarkPressure::NORMAL) == 4 * baseRatio,
        "fourth outstanding level must reach the configured assist cap");
    Expect(
        ComputeMSMarkDynamicAssistRatioQ16(
            baseRatio, 4096, maxMultiplier, MSMarkPressure::NORMAL) == 4 * baseRatio,
        "large outstanding backlog must remain capped");
    Expect(
        ComputeMSMarkDynamicAssistRatioQ16(
            baseRatio, 9, maxMultiplier, MSMarkPressure::NORMAL) == 2 * baseRatio,
        "falling outstanding must reduce the ratio by the same level policy");
    Expect(
        ComputeMSMarkDynamicAssistRatioQ16(
            baseRatio, 0, maxMultiplier, MSMarkPressure::NORMAL) == baseRatio,
        "falling outstanding must not reduce the ratio below its cycle-start floor");
    Expect(
        ComputeMSMarkDynamicAssistRatioQ16(
            0, 4096, maxMultiplier, MSMarkPressure::NORMAL) == 0,
        "disabled assist must remain disabled under backlog pressure");
    Expect(
        ComputeMSMarkDynamicAssistRatioQ16(
            baseRatio, 1, maxMultiplier, MSMarkPressure::CRITICAL) == 4 * baseRatio,
        "critical pressure must select the maximum assist multiplier");
}

void TestHelperTargetDoesNotDoubleCountReadyTasks()
{
    Expect(
        ComputeMSMarkHelperTarget(4096, 3, MSMarkPressure::NORMAL) == 3,
        "normal mark must scale helpers from outstanding ledger work");
    Expect(
        ComputeMSMarkHelperTarget(4096, 3, MSMarkPressure::HIGH) == 3,
        "high pressure must not reduce an already wider helper target");
    Expect(
        ComputeMSMarkHelperTarget(4096, 3, MSMarkPressure::CRITICAL) == 3,
        "critical pressure must activate all available helpers");
    Expect(
        ComputeMSMarkHelperTarget(1, 3, MSMarkPressure::CRITICAL) == 1,
        "helper target must not exceed outstanding tasks");
}

} // namespace

int main()
{
    TestFixedIntegerCost();
    TestFullCycleEwma();
    TestAssistRatio();
    TestDynamicAssistRatioTracksHelperOutstandingLevels();
    TestHelperTargetDoesNotDoubleCountReadyTasks();
    return 0;
}
