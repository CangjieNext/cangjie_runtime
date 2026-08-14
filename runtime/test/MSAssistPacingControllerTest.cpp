// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <cstdlib>
#include <iostream>

#include "src/Heap/MCollector/MSAssistPacing.h"

namespace {

using namespace MapleRuntime;

void Expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void TestMarkPacingOwnsCyclePlan()
{
    MSMarkAssistPacing pacing;
    pacing.UpdateForCompletedCycle(1024, 32768, 4, false);

    MSMarkAssistPlan normal =
        pacing.ActivateForCurrentCycle(MSMarkPressure::NORMAL);
    Expect(normal.baseRatio == 288,
           "pacing must derive the next normal base ratio from cycle work density");
    Expect(normal.maxMultiplier == 4,
           "pacing must retain the normalized assist multiplier cap");
    Expect(pacing.GetPublishedRateScaled() == 288,
           "normal activation must publish one times the planned base ratio");

    pacing.PublishRateForMultiplier(normal, 2);
    Expect(pacing.GetPublishedRateScaled() == 576,
           "controller multiplier updates must publish through pacing state");
    pacing.DeactivateCurrentCycle();
    Expect(pacing.GetPublishedRateScaled() == 0,
           "deactivation must prevent assists outside concurrent mark");

    MSMarkAssistPlan critical =
        pacing.ActivateForCurrentCycle(MSMarkPressure::CRITICAL);
    Expect(pacing.GetPublishedRateScaled() == 1152,
           "critical activation must publish the planned maximum multiplier");
    Expect(critical.baseRatio == normal.baseRatio &&
               critical.maxMultiplier == normal.maxMultiplier,
           "activation must return the immutable next-cycle plan");
}

void TestMarkPacingOwnsCompletedAssistWork()
{
    MSMarkAssistPacing pacing;
    pacing.AccountCompletedAssistWork(17);
    pacing.AccountCompletedAssistWork(23);
    Expect(pacing.DrainCompletedAssistWork() == 40,
           "pacing must aggregate completed mutator-assist work");
    Expect(pacing.DrainCompletedAssistWork() == 0,
           "draining completed mutator-assist work must be one-shot");
}

void TestMarkPacingOwnsControllerAndPressure()
{
    MSMarkControllerState controller(
        16, 128, 7, 4, MSMarkPressure::NORMAL);
    Expect(controller.HelperTarget() == 0,
           "pacing controller must preserve small-backlog helper admission");

    controller.ObserveSchedulerSample(16, 128, 20000);
    Expect(controller.Pressure() == MSMarkPressure::NORMAL,
           "pacing controller must reject a transient high-pressure sample");
    Expect(ClassifyMSMarkElapsedPressure(100000, true) ==
               MSMarkPressure::CRITICAL,
           "pacing must classify a 100ms active mark as critical");
}

} // namespace

int main()
{
    TestMarkPacingOwnsCyclePlan();
    TestMarkPacingOwnsControllerAndPressure();
    TestMarkPacingOwnsCompletedAssistWork();
    return 0;
}
