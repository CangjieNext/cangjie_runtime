#include <cstdio>
#include <cstdlib>

#define MRT_USE_MARK_SWEEP 1

#include "src/Heap/MCollector/MSAssistPacing.h"
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

void TestAssistAdmissionRequiresSustainedBacklog()
{
    Expect(!ShouldAdmitMSMarkAssist(0, 4096, 4),
           "Assist must reject an empty task backlog");
    Expect(!ShouldAdmitMSMarkAssist(8, 0, 4),
           "Assist must reject an empty work-word backlog");
    Expect(!ShouldAdmitMSMarkAssist(6, 639, 4),
           "Assist must leave shallow work to main plus helpers");
    Expect(ShouldAdmitMSMarkAssist(7, 1, 4),
           "extra tasks beyond the reserved workers must admit Assist");
    Expect(ShouldAdmitMSMarkAssist(1, 640, 4),
           "deep work-word backlog must admit Assist");
    Expect(ShouldAdmitMSMarkAssist(3, 128, 0),
           "single-worker Mark must admit Assist beyond its reserve");
}

void TestAssistCreditPaysFutureDebtAndPreservesFraction()
{
    MSMarkAssistDebtState state;
    state.debtScaled = 4096 * kMSAssistRatioScale + 7;
    state.creditWU = 1024;
    state.ConsumeCredit();
    Expect(state.debtScaled == 3072 * kMSAssistRatioScale + 7,
           "Assist credit must preserve fractional scaled debt");
    Expect(state.creditWU == 0,
           "consumed Assist credit must not be reused");

    state.debtScaled = 2048 * kMSAssistRatioScale + 5;
    state.creditWU = 0;
    state.AccountCompletedWork(5000);
    Expect(state.debtScaled == 5,
           "completed Assist work must pay only whole debt WU");
    Expect(state.creditWU == 2952,
           "whole-Region overshoot must become future Assist credit");

    state.debtScaled = 0;
    state.creditWU = std::numeric_limits<uint64_t>::max() - 1;
    state.AccountCompletedWork(4);
    Expect(state.creditWU == std::numeric_limits<uint64_t>::max(),
           "Assist credit accumulation must saturate");
}

void TestFullCycleEwma()
{
    MSMarkAssistPacing pacing;
    pacing.UpdateForCompletedCycle(1024, 32768, 1, false);
    MSMarkAssistPlan plan =
        pacing.ActivateForCurrentCycle(MSMarkPressure::NORMAL);
    uint64_t expectedDensity = UpdateMSWUEwmaOneEighth(
        MS_WU_INITIAL_DENSITY_Q16, MSWUQ16Divide(1024, 32768));
    Expect(plan.baseRatio ==
               ResolveMSMarkBaseAssistRatioQ16(expectedDensity, false),
           "pacing must retain the density EWMA internally");
}

void TestAssistRatio()
{
    Expect(
        ResolveMSMarkBaseAssistRatioQ16(MS_WU_Q16_ONE, false) ==
            MS_WU_Q16_ONE / 4,
        "normal assist ratio mismatch");
    Expect(
        ResolveMSMarkBaseAssistRatioQ16(MS_WU_Q16_ONE, true) == 0,
        "disabled assist must publish no allocation charge");
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

    controller.ObserveSchedulerSample(16, 1024, 0);
    Expect(controller.HelperTarget() == 0,
           "one high-work observation must not wake helpers");
    controller.ObserveSchedulerSample(16, 1024, 0);
    controller.ObserveSchedulerSample(16, 1024, 0);
    Expect(controller.HelperTarget() == 0,
           "three high-work observations must remain inside up hysteresis");
    controller.ObserveSchedulerSample(16, 1024, 0);
    Expect(controller.HelperTarget() == 1,
           "four sustained high-work observations must add only one helper");

    MSMarkControllerState draining(
        16, 1024, 7, 4, MSMarkPressure::NORMAL);
    Expect(draining.HelperTarget() == 2,
           "normal mark must soft-start at two helpers");
    for (size_t window = 0; window < 6; ++window) {
        draining.ObserveSchedulerSample(16, 0, 0);
        Expect(draining.HelperTarget() == 2,
               "filtered draining work must not park helpers abruptly");
    }
    draining.ObserveSchedulerSample(16, 0, 0);
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
        controller.ObserveSchedulerSample(64, 4096, 0);
        Expect(controller.HelperTarget() == 2,
               "helper growth must require four sustained backlog windows");
    }
    controller.ObserveSchedulerSample(64, 4096, 0);
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

    controller.ObserveSchedulerSample(32, 4096, 0);
    Expect(controller.AssistMultiplier() == 1,
           "one non-draining window must not increase Assist");
    controller.ObserveSchedulerSample(32, 4096, 0);
    Expect(controller.AssistMultiplier() == 2,
           "two non-draining windows after stable helpers must add one Assist level");
    controller.ObserveSchedulerSample(32, 4096, 0);
    Expect(controller.AssistMultiplier() == 2,
           "Assist must not increase on every control window");
    controller.ObserveSchedulerSample(32, 4096, 0);
    Expect(controller.AssistMultiplier() == 3,
           "persistent backlog must raise Assist one level at a time");

    controller.ObserveSchedulerSample(32, 0, 0);
    Expect(controller.AssistMultiplier() == 3,
           "one draining window must stay inside Assist down hysteresis");
    controller.ObserveSchedulerSample(32, 0, 0);
    Expect(controller.AssistMultiplier() == 2,
           "two draining windows must reduce Assist by one level");
}

void TestMarkPressureEscalatesOnElapsedTime()
{
    Expect(ClassifyMSMarkElapsedPressure(19999, true) == MSMarkPressure::NORMAL,
           "short Mark cycle must remain normal");
    Expect(ClassifyMSMarkElapsedPressure(20000, true) == MSMarkPressure::HIGH,
           "20ms Mark cycle must raise high pressure");
    Expect(ClassifyMSMarkElapsedPressure(99999, true) == MSMarkPressure::HIGH,
           "sub-100ms Mark cycle must remain high pressure");
    Expect(ClassifyMSMarkElapsedPressure(100000, true) == MSMarkPressure::CRITICAL,
           "100ms Mark cycle must raise critical pressure");
    Expect(ClassifyMSMarkElapsedPressure(1000000, false) == MSMarkPressure::NORMAL,
           "elapsed time without mark work must not raise pressure");
}

void TestMarkControllerRequiresSustainedElapsedPressure()
{
    MSMarkControllerState highController(
        16, 128, 7, 4, MSMarkPressure::NORMAL);
    highController.ObserveSchedulerSample(16, 128, 20000);
    Expect(highController.Pressure() == MSMarkPressure::NORMAL,
           "one high-pressure sample must not expand mark helpers");
    highController.ObserveSchedulerSample(16, 128, 20000);
    Expect(highController.Pressure() == MSMarkPressure::NORMAL,
           "two high-pressure samples must remain gated");
    highController.ObserveSchedulerSample(16, 128, 20000);
    Expect(highController.Pressure() == MSMarkPressure::HIGH,
           "three high-pressure samples must enable high pressure");

    MSMarkControllerState resetController(
        16, 128, 7, 4, MSMarkPressure::NORMAL);
    resetController.ObserveSchedulerSample(16, 128, 20000);
    resetController.ObserveSchedulerSample(16, 128, 0);
    resetController.ObserveSchedulerSample(16, 128, 20000);
    Expect(resetController.Pressure() == MSMarkPressure::NORMAL,
           "a normal sample must reset a partial high-pressure streak");

    MSMarkControllerState criticalController(
        64, 4096, 7, 4, MSMarkPressure::NORMAL);
    criticalController.ObserveSchedulerSample(64, 4096, 100000);
    Expect(criticalController.Pressure() == MSMarkPressure::NORMAL,
           "one critical-pressure sample must not activate every helper");
    criticalController.ObserveSchedulerSample(64, 4096, 100000);
    Expect(criticalController.Pressure() == MSMarkPressure::CRITICAL,
           "two critical-pressure samples must activate critical pressure");
}

void TestMarkControllerEscalatesToCriticalPressure()
{
    MSMarkControllerState controller(
        64, 4096, 7, 4, MSMarkPressure::NORMAL);
    controller.ObserveSchedulerSample(64, 4096, 100000);
    controller.ObserveSchedulerSample(64, 4096, 100000);
    Expect(controller.HelperTarget() == 7,
           "critical pressure escalation must immediately activate all helpers");
    Expect(controller.AssistMultiplier() == 4,
           "critical pressure escalation must immediately use the Assist cap");
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
    TestAssistAdmissionRequiresSustainedBacklog();
    TestAssistCreditPaysFutureDebtAndPreservesFraction();
    TestFullCycleEwma();
    TestAssistRatio();
    TestHelperTargetTracksActualWork();
    TestMarkControllerAppliesHelperHysteresis();
    TestNormalMarkSoftStartsAndRampsOnSustainedBacklog();
    TestMarkControllerCascadesAssistAfterStableHelpers();
    TestMarkPressureEscalatesOnElapsedTime();
    TestMarkControllerRequiresSustainedElapsedPressure();
    TestMarkControllerEscalatesToCriticalPressure();
    TestCriticalMarkControllerBypassesHysteresis();
    return 0;
}
