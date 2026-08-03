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

void TestAssistSlicePolicyStartsAtTwoKForOneHundredMicroseconds()
{
    Expect(MS_WU_ASSIST_THRESHOLD == 2048,
           "Assist debt trigger must start at 2K WU");
    Expect(MS_WU_ASSIST_WALL_BUDGET_US == 100,
           "Assist wall budget must be 100us");
    Expect(ComputeMSMarkAssistSliceBudget(1) == 2048,
           "small Assist debt must receive a 2K WU slice");
    Expect(ComputeMSMarkAssistSliceBudget(4096) == 4096,
           "large Assist debt must preserve the requested WU slice");
}

void TestAssistCreditPaysFutureDebtAndPreservesFraction()
{
    constexpr uint64_t scale = 16;
    uint64_t debtScaled = 4096 * scale + 7;
    uint64_t creditWU = 1024;
    ConsumeMSMarkAssistCredit(debtScaled, creditWU, scale);
    Expect(debtScaled == 3072 * scale + 7,
           "Assist credit must preserve fractional scaled debt");
    Expect(creditWU == 0,
           "consumed Assist credit must not be reused");

    debtScaled = 2048 * scale + 5;
    creditWU = 0;
    AccountMSMarkAssistWork(debtScaled, creditWU, 5000, scale);
    Expect(debtScaled == 5,
           "completed Assist work must pay only whole debt WU");
    Expect(creditWU == 2952,
           "whole-Region overshoot must become future Assist credit");

    debtScaled = 0;
    creditWU = std::numeric_limits<uint64_t>::max() - 1;
    AccountMSMarkAssistWork(debtScaled, creditWU, 4, scale);
    Expect(creditWU == std::numeric_limits<uint64_t>::max(),
           "Assist credit accumulation must saturate");
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

void TestMarkTelemetryShareAndRate()
{
    Expect(
        ComputeMSMarkShareBasisPoints(250, 1000) == 2500,
        "mark work share must use basis points");
    Expect(
        ComputeMSMarkShareBasisPoints(1, 0) == 0,
        "empty mark work must report zero share");
    Expect(
        ComputeMSMarkRateQ16(1000, 40) == 25 * MS_WU_Q16_ONE,
        "mark rate must report WorkUnits per microsecond");
    Expect(
        ComputeMSMarkRateQ16(1000, 0) == 0,
        "zero active time must report zero rate");
}

void TestHelperTargetTracksActualWork()
{
    Expect(
        ComputeMSMarkHelperTarget(4096, 4096, 3, MSMarkPressure::NORMAL) == 3,
        "normal mark must scale helpers from outstanding ledger work");
    Expect(
        ComputeMSMarkHelperTarget(4096, 4096, 3, MSMarkPressure::HIGH) == 3,
        "high pressure must not reduce an already wider helper target");
    Expect(
        ComputeMSMarkHelperTarget(4096, 4096, 3, MSMarkPressure::CRITICAL) == 3,
        "critical pressure must activate all available helpers");
    Expect(
        ComputeMSMarkHelperTarget(1, 64, 3, MSMarkPressure::CRITICAL) == 1,
        "helper target must not exceed outstanding tasks");

    Expect(
        ComputeMSMarkHelperTarget(16, 128, 7, MSMarkPressure::NORMAL) == 0,
        "GC main must absorb the first 128 pending bitmap words");
    Expect(
        ComputeMSMarkHelperTarget(16, 129, 7, MSMarkPressure::NORMAL) == 1,
        "the second 128-word share must activate one helper");
    Expect(
        ComputeMSMarkHelperTarget(16, 256, 7, MSMarkPressure::NORMAL) == 1,
        "a full two-participant share must not activate a third participant");
    Expect(
        ComputeMSMarkHelperTarget(16, 257, 7, MSMarkPressure::NORMAL) == 2,
        "work beyond two participant shares must activate a second helper");
    Expect(
        ComputeMSMarkHelperTarget(16, 1024, 7, MSMarkPressure::NORMAL) == 7,
        "eight participant shares must activate all seven helpers");
    Expect(
        ComputeMSMarkHelperTarget(2, 1024, 7, MSMarkPressure::NORMAL) == 1,
        "ready task parallelism must reserve one task for GC main");
    Expect(
        ComputeMSMarkHelperTarget(16, 1, 7, MSMarkPressure::NORMAL) == 0,
        "a nonempty sub-share backlog must stay on GC main");
    Expect(
        ComputeMSMarkHelperTarget(16, 0, 7, MSMarkPressure::NORMAL) == 0,
        "zero remaining scan work must park all helpers");
}

void TestMarkControllerAppliesHelperHysteresis()
{
    Expect(MS_MARK_CONTROL_SLICES_PER_UPDATE == 4,
           "mark control must sample once per four GC-main slices");
    MSMarkControllerState controller(
        16, 128, 7, 4, MSMarkPressure::NORMAL);
    Expect(controller.HelperTarget() == 0,
           "small initial backlog must stay on GC main");
    Expect(controller.AssistMultiplier() == 1,
           "normal mark must start at the base Assist ratio");

    controller.Observe(16, 1024);
    Expect(controller.HelperTarget() == 0,
           "one high-work observation must not wake helpers");
    controller.Observe(16, 1024);
    controller.Observe(16, 1024);
    Expect(controller.HelperTarget() == 0,
           "three high-work observations must remain inside up hysteresis");
    controller.Observe(16, 1024);
    Expect(controller.HelperTarget() == 1,
           "four sustained high-work observations must add only one helper");

    MSMarkControllerState draining(
        16, 1024, 7, 4, MSMarkPressure::NORMAL);
    Expect(draining.HelperTarget() == 2,
           "normal mark must soft-start at two helpers");
    for (size_t window = 0; window < 6; ++window) {
        draining.Observe(16, 0);
        Expect(draining.HelperTarget() == 2,
               "filtered draining work must not park helpers abruptly");
    }
    draining.Observe(16, 0);
    Expect(draining.HelperTarget() == 1,
           "sustained draining must remove only one helper");
}

void TestNormalMarkSoftStartsAndRampsOnSustainedBacklog()
{
    MSMarkControllerState controller(
        64, 4096, 7, 4, MSMarkPressure::NORMAL);
    Expect(controller.HelperTarget() == 2,
           "large normal backlog must not activate all helpers at mark start");
    for (size_t window = 0; window < 3; ++window) {
        controller.Observe(64, 4096);
        Expect(controller.HelperTarget() == 2,
               "helper growth must require four sustained backlog windows");
    }
    controller.Observe(64, 4096);
    Expect(controller.HelperTarget() == 3,
           "sustained backlog must grow helper width one step at a time");
}

void TestMarkControllerCascadesAssistAfterStableHelpers()
{
    MSMarkControllerState controller(
        32, 4096, 2, 4, MSMarkPressure::NORMAL);
    Expect(controller.HelperTarget() == 2,
           "assist cascade test must start with its available helpers active");
    Expect(controller.AssistMultiplier() == 1,
           "task count must not saturate Assist at mark start");

    controller.Observe(32, 4096);
    Expect(controller.AssistMultiplier() == 1,
           "one non-draining window must not increase Assist");
    controller.Observe(32, 4096);
    Expect(controller.AssistMultiplier() == 2,
           "two non-draining windows after stable helpers must add one Assist level");
    controller.Observe(32, 4096);
    Expect(controller.AssistMultiplier() == 2,
           "Assist must not increase on every control window");
    controller.Observe(32, 4096);
    Expect(controller.AssistMultiplier() == 3,
           "persistent backlog must raise Assist one level at a time");

    controller.Observe(32, 0);
    Expect(controller.AssistMultiplier() == 3,
           "one draining window must stay inside Assist down hysteresis");
    controller.Observe(32, 0);
    Expect(controller.AssistMultiplier() == 2,
           "two draining windows must reduce Assist by one level");
}

void TestCriticalMarkControllerBypassesHysteresis()
{
    MSMarkControllerState controller(
        7, 1, 7, 4, MSMarkPressure::CRITICAL);
    Expect(controller.HelperTarget() == 7,
           "critical mark must activate every claimable helper");
    Expect(controller.AssistMultiplier() == 4,
           "critical mark must immediately use the Assist cap");
}

} // namespace

int main()
{
    TestFixedIntegerCost();
    TestAssistSlicePolicyStartsAtTwoKForOneHundredMicroseconds();
    TestAssistCreditPaysFutureDebtAndPreservesFraction();
    TestFullCycleEwma();
    TestAssistRatio();
    TestMarkTelemetryShareAndRate();
    TestHelperTargetTracksActualWork();
    TestMarkControllerAppliesHelperHysteresis();
    TestNormalMarkSoftStartsAndRampsOnSustainedBacklog();
    TestMarkControllerCascadesAssistAfterStableHelpers();
    TestCriticalMarkControllerBypassesHysteresis();
    return 0;
}
