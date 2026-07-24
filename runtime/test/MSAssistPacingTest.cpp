#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSAssistPacing.h"

namespace {

using MapleRuntime::ApplyMSAssistDisableOverride;
using MapleRuntime::ComputeMSAssistBacklogRatioScaled;
using MapleRuntime::ComputeMSAssistBaseRatioScaled;
using MapleRuntime::EvaluateMSAssistBaseRatio;
using MapleRuntime::UpdateMSAssistBacklogEMA;
using MapleRuntime::kMSAssistBaseRatioPercentDefault;

void ExpectEqual(uint64_t actual, uint64_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected %llu, got %llu\n", message,
                     static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual));
        std::abort();
    }
}

void TestBaseRatioPercentKeepsFractionalPrecision()
{
    constexpr size_t prevMarked = 1500;
    constexpr size_t allocatedBytes = 1000000;
    uint64_t full = ComputeMSAssistBaseRatioScaled(prevMarked, allocatedBytes, 100);
    uint64_t half = ComputeMSAssistBaseRatioScaled(prevMarked, allocatedBytes, 50);

    ExpectEqual(full, 98, "100 percent base ratio");
    ExpectEqual(half, 49, "50 percent base ratio");
}

void TestBaseRatioPercentDefaultsToHalfPacing()
{
    ExpectEqual(kMSAssistBaseRatioPercentDefault, 50, "default base ratio percent");
}

void TestDiagnosticDisableOverrideZerosAssistRatio()
{
    ExpectEqual(ApplyMSAssistDisableOverride(12345, true), 0, "disabled assist ratio");
}

void TestDiagnosticDisableOverridePreservesEnabledRatio()
{
    ExpectEqual(ApplyMSAssistDisableOverride(12345, false), 12345, "enabled assist ratio");
}

void TestBacklogRatioIncreasesContinuously()
{
    constexpr uint64_t baseRatio = 1000;
    ExpectEqual(ComputeMSAssistBacklogRatioScaled(baseRatio, 4.0, 16, 4), 1250, "quarter backlog step");
    ExpectEqual(ComputeMSAssistBacklogRatioScaled(baseRatio, 8.0, 16, 4), 1500, "half backlog step");
    ExpectEqual(ComputeMSAssistBacklogRatioScaled(baseRatio, 12.0, 16, 4), 1750,
                "three-quarter backlog step");
    ExpectEqual(ComputeMSAssistBacklogRatioScaled(baseRatio, 16.0, 16, 4), 2000, "full backlog step");
    ExpectEqual(ComputeMSAssistBacklogRatioScaled(baseRatio, 1000.0, 16, 4), 4000, "maximum multiplier");
}

void TestBacklogEMAStartsImmediatelyAndThenSmooths()
{
    double smoothed = UpdateMSAssistBacklogEMA(0.0, 32, false);
    ExpectEqual(static_cast<uint64_t>(smoothed), 32, "first backlog sample");

    smoothed = UpdateMSAssistBacklogEMA(smoothed, 0, true);
    ExpectEqual(static_cast<uint64_t>(smoothed), 28, "decayed backlog sample");
}

void TestBaseRatioFeedbackInitializesFromRawTarget()
{
    auto decision = EvaluateMSAssistBaseRatio(200, 1, false, 0, 0);
    ExpectEqual(decision.nextRatio, 200, "first meaningful base ratio");
}

void TestBaseRatioDecreaseFollowsRawTargetImmediately()
{
    auto decision = EvaluateMSAssistBaseRatio(100, 200, true, 25, 75);
    ExpectEqual(decision.nextRatio, 100, "base ratio decrease");
}

void TestBaseRatioIncreaseIsLimitedToTwentyFivePercent()
{
    auto decision = EvaluateMSAssistBaseRatio(400, 200, true, 20, 80);
    ExpectEqual(decision.nextRatio, 250, "base ratio rise limit");
    ExpectEqual(decision.riseLimited, 1, "rise limit diagnostic");
}

void TestAssistShareBelowNinetyPercentDoesNotFeedBack()
{
    auto decision = EvaluateMSAssistBaseRatio(300, 200, true, 700, 300);
    ExpectEqual(decision.assistShareBasisPoints, 7000, "assist share below threshold");
    ExpectEqual(decision.feedbackLimit, 300, "below-threshold feedback limit");
    ExpectEqual(decision.nextRatio, 250, "below-threshold ratio keeps rise limit");
    ExpectEqual(decision.shareLimited, 0, "below-threshold share diagnostic");
}

void TestAssistShareEqualToNinetyPercentDoesNotFeedBack()
{
    auto decision = EvaluateMSAssistBaseRatio(300, 200, true, 900, 100);
    ExpectEqual(decision.assistShareBasisPoints, 9000, "assist share at threshold");
    ExpectEqual(decision.feedbackLimit, 300, "threshold feedback limit");
    ExpectEqual(decision.nextRatio, 250, "threshold ratio keeps rise limit");
    ExpectEqual(decision.shareLimited, 0, "threshold share diagnostic");
}

void TestAssistShareAboveNinetyPercentFeedsBackTowardEightyPercent()
{
    auto decision = EvaluateMSAssistBaseRatio(300, 200, true, 901, 99);
    ExpectEqual(decision.assistShareBasisPoints, 9010, "assist share above threshold");
    ExpectEqual(decision.feedbackLimit, 177, "above-threshold feedback limit");
    ExpectEqual(decision.nextRatio, 177, "above-threshold limited ratio");
    ExpectEqual(decision.shareLimited, 1, "above-threshold share diagnostic");
}

void TestLowAssistShareDoesNotBypassRiseLimit()
{
    auto decision = EvaluateMSAssistBaseRatio(400, 200, true, 10, 90);
    ExpectEqual(decision.nextRatio, 250, "low share should not force extra assist");
    ExpectEqual(decision.shareLimited, 0, "low share diagnostic");
}

} // namespace

int main()
{
    TestBaseRatioPercentKeepsFractionalPrecision();
    TestBaseRatioPercentDefaultsToHalfPacing();
    TestDiagnosticDisableOverrideZerosAssistRatio();
    TestDiagnosticDisableOverridePreservesEnabledRatio();
    TestBacklogRatioIncreasesContinuously();
    TestBacklogEMAStartsImmediatelyAndThenSmooths();
    TestBaseRatioFeedbackInitializesFromRawTarget();
    TestBaseRatioDecreaseFollowsRawTargetImmediately();
    TestBaseRatioIncreaseIsLimitedToTwentyFivePercent();
    TestAssistShareBelowNinetyPercentDoesNotFeedBack();
    TestAssistShareEqualToNinetyPercentDoesNotFeedBack();
    TestAssistShareAboveNinetyPercentFeedsBackTowardEightyPercent();
    TestLowAssistShareDoesNotBypassRiseLimit();
    return 0;
}
