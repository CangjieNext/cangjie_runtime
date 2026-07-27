// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "src/Heap/MCollector/MSMarkTerminationPolicy.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using MapleRuntime::MSMarkPressurePolicy;
using MapleRuntime::MSMarkTerminationReason;
using MapleRuntime::GetMSMarkTerminationReasonName;
using MapleRuntime::ScaleMSAssistRatioForPressure;

void ExpectEqual(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n", message, expected, actual);
        std::abort();
    }
}

void ExpectTrue(bool actual, const char* message)
{
    if (!actual) {
        std::fprintf(stderr, "%s: expected=true actual=false\n", message);
        std::abort();
    }
}

void ExpectFalse(bool actual, const char* message)
{
    if (actual) {
        std::fprintf(stderr, "%s: expected=false actual=true\n", message);
        std::abort();
    }
}

void TestBacklogRaisesOneLevelPerObjectQuantum()
{
    constexpr size_t quantum = 1ULL << 20;
    ExpectEqual(MSMarkPressurePolicy::kCheckpointObjects, quantum,
                "checkpoint object interval");
    ExpectEqual(MSMarkPressurePolicy::BacklogLevel(0), 0, "zero backlog");
    ExpectEqual(MSMarkPressurePolicy::BacklogLevel(1), 1, "first backlog level");
    ExpectEqual(MSMarkPressurePolicy::BacklogLevel(quantum), 1, "one object quantum");
    ExpectEqual(MSMarkPressurePolicy::BacklogLevel(quantum + 1), 2, "second backlog level");
    ExpectEqual(MSMarkPressurePolicy::BacklogLevel(10 * quantum), 10, "tenth backlog level");
}

void TestOneLevelControlsBothHelperAndAssistTargets()
{
    constexpr size_t quantum = 1ULL << 20;
    MSMarkPressurePolicy policy(15, 4, false, 2 * quantum + 1);
    ExpectEqual(policy.GetPressureLevel(), 3, "initial backlog pressure");
    ExpectEqual(policy.GetTargetHelpers(), 3, "helper target");
    ExpectEqual(policy.GetAssistMultiplier(), 3, "assist target");

    MSMarkPressurePolicy capped(15, 4, false, 10 * quantum);
    ExpectEqual(capped.GetPressureLevel(), 10, "higher pressure");
    ExpectEqual(capped.GetTargetHelpers(), 10, "helper keeps scaling");
    ExpectEqual(capped.GetAssistMultiplier(), 4, "assist multiplier cap");
}

void TestNonDecreasingDebtRaisesOneLevelAndForcesAfterThreeWindows()
{
    constexpr size_t quantum = 1ULL << 20;
    MSMarkPressurePolicy policy(15, 4, false, 5 * quantum);

    auto first = policy.Checkpoint(5 * quantum);
    ExpectEqual(first.pressureLevel, 6, "first stalled window raises pressure");
    ExpectEqual(first.noProgressWindows, 1, "first stalled window");
    ExpectFalse(first.forceForNoProgress, "first stalled window must not force");

    auto second = policy.Checkpoint(5 * quantum + 1);
    ExpectEqual(second.pressureLevel, 7, "growing debt raises pressure");
    ExpectEqual(second.noProgressWindows, 2, "second stalled window");
    ExpectFalse(second.forceForNoProgress, "second stalled window must not force");

    auto third = policy.Checkpoint(5 * quantum + 1);
    ExpectEqual(third.pressureLevel, 8, "third stalled window raises pressure");
    ExpectEqual(third.noProgressWindows, 3, "third stalled window");
    ExpectTrue(third.forceForNoProgress, "third stalled window forces STW");
}

void TestDecreasingDebtResetsNoProgressWindows()
{
    constexpr size_t quantum = 1ULL << 20;
    MSMarkPressurePolicy policy(15, 4, false, 5 * quantum);
    (void)policy.Checkpoint(5 * quantum);
    auto decision = policy.Checkpoint(4 * quantum);

    ExpectEqual(decision.noProgressWindows, 0, "progress resets stalled windows");
    ExpectFalse(decision.forceForNoProgress, "progress must not force STW");
}

void TestOOMStartsAtMaximumPressure()
{
    MSMarkPressurePolicy policy(15, 4, true, 1);
    ExpectEqual(policy.GetPressureLevel(), 15, "OOM pressure level");
    ExpectEqual(policy.GetTargetHelpers(), 15, "OOM helpers");
    ExpectEqual(policy.GetAssistMultiplier(), 4, "OOM assist multiplier");
}

void TestAssistPressureScalingPreservesDisableAndSaturates()
{
    ExpectEqual(ScaleMSAssistRatioForPressure(0, 4), 0, "disabled assist stays disabled");
    ExpectEqual(ScaleMSAssistRatioForPressure(1000, 3), 3000, "assist pressure multiplier");
    ExpectEqual(ScaleMSAssistRatioForPressure(
                    static_cast<uint64_t>(-1) - 1, 2),
                static_cast<uint64_t>(-1),
                "assist ratio saturates");
}

void TestTerminationReasonNamesExposeEverySTWBranch()
{
    struct ReasonName {
        MSMarkTerminationReason reason;
        const char* name;
    };
    const ReasonName expected[] = {
        { MSMarkTerminationReason::NATURAL, "natural" },
        { MSMarkTerminationReason::DEBT_STALLED, "debt-stalled" },
        { MSMarkTerminationReason::TIME_BUDGET, "time-budget" },
    };
    for (const ReasonName& item : expected) {
        if (std::string(GetMSMarkTerminationReasonName(item.reason)) != item.name) {
            std::fprintf(stderr, "unexpected STW branch name for reason=%zu\n",
                         static_cast<size_t>(item.reason));
            std::abort();
        }
    }
}

} // namespace

int main()
{
    TestBacklogRaisesOneLevelPerObjectQuantum();
    TestOneLevelControlsBothHelperAndAssistTargets();
    TestNonDecreasingDebtRaisesOneLevelAndForcesAfterThreeWindows();
    TestDecreasingDebtResetsNoProgressWindows();
    TestOOMStartsAtMaximumPressure();
    TestAssistPressureScalingPreservesDisableAndSaturates();
    TestTerminationReasonNamesExposeEverySTWBranch();
    return 0;
}
