#include <cstdio>
#include <cstdlib>

#define MRT_USE_MARK_SWEEP 1

#include "src/Heap/MCollector/MSWorkUnit.h"

namespace {

using namespace MapleRuntime;

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
    TestHelperTargetDoesNotDoubleCountReadyTasks();
    return 0;
}
