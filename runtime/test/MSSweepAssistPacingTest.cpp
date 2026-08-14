#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

#define MRT_USE_MARK_SWEEP
#include "src/Heap/MAllocator/MSSweepPressurePolicy.h"
#include "src/Heap/MCollector/MSAssistPacing.h"

namespace {

using MapleRuntime::ComputeMSSweepAssistRatioScaled;
using MapleRuntime::ComputeMSSweepAssistDirectRegions;
using MapleRuntime::ComputeMSSweepRunwayBytes;
using MapleRuntime::EvaluateSweepPressure;
using MapleRuntime::MSSweepAssistResult;
using MapleRuntime::MSSweepCreditPool;
using MapleRuntime::ScaleMSSweepAssistRatioScaled;
using MapleRuntime::SweepPressureMode;
using MapleRuntime::MSSweepAssistDebtState;
using MapleRuntime::kMSSweepAssistRatioScale;

void ExpectEqual(uint64_t actual, uint64_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected %llu, got %llu\n", message,
                     static_cast<unsigned long long>(expected),
                     static_cast<unsigned long long>(actual));
        std::abort();
    }
}

void ExpectTrue(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void ExpectPressure(size_t unsweptBytes, size_t sweptBytes, size_t thresholdBytes,
                    bool forcedHigh, SweepPressureMode previousMode,
                    SweepPressureMode expected, const char* message)
{
    SweepPressureMode actual = EvaluateSweepPressure(
        unsweptBytes, sweptBytes, thresholdBytes, forcedHigh, previousMode);
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected mode %d, got %d\n", message,
                     static_cast<int>(expected), static_cast<int>(actual));
        std::abort();
    }
}

void TestRatioKeepsFractionalRegionPrecision()
{
    ExpectEqual(ComputeMSSweepAssistRatioScaled(3, 4),
                kMSSweepAssistRatioScale * 3 / 4,
                "three quarters of a sweep region per allocation");
    ExpectEqual(ComputeMSSweepAssistRatioScaled(0, 4), 0,
                "empty backlog disables sweep debt");
    ExpectEqual(ComputeMSSweepAssistRatioScaled(4, 0),
                kMSSweepAssistRatioScale * 4,
                "zero runway is clamped to one region");
}

void TestPressureScalingSaturates()
{
    ExpectEqual(ScaleMSSweepAssistRatioScaled(10, SweepPressureMode::LOW), 10,
                "low pressure ratio");
    ExpectEqual(ScaleMSSweepAssistRatioScaled(10, SweepPressureMode::MEDIUM), 40,
                "medium pressure ratio");
    ExpectEqual(ScaleMSSweepAssistRatioScaled(10, SweepPressureMode::HIGH), 100,
                "high pressure ratio");
    ExpectEqual(ScaleMSSweepAssistRatioScaled(std::numeric_limits<uint64_t>::max() - 1,
                                              SweepPressureMode::HIGH),
                std::numeric_limits<uint64_t>::max(),
                "pressure ratio saturation");
}

void TestPressureUsesRemainingRunwayAndHysteresis()
{
    ExpectEqual(ComputeMSSweepRunwayBytes(100, 20), 80,
                "runway subtracts swept-retained bytes");
    ExpectEqual(ComputeMSSweepRunwayBytes(100, 100), 0,
                "runway is zero at threshold");
    ExpectEqual(ComputeMSSweepRunwayBytes(100, 101), 0,
                "runway subtraction cannot underflow");

    // R = 80: LOW enters MEDIUM at R/2 and HIGH at R.
    ExpectPressure(39, 20, 100, false, SweepPressureMode::LOW, SweepPressureMode::LOW,
                   "below half runway remains low");
    ExpectPressure(40, 20, 100, false, SweepPressureMode::LOW, SweepPressureMode::MEDIUM,
                   "half runway enters medium");
    ExpectPressure(80, 20, 100, false, SweepPressureMode::LOW, SweepPressureMode::HIGH,
                   "full runway enters high");

    // Downgrades use R/2 and R/4 hysteresis.
    ExpectPressure(40, 20, 100, false, SweepPressureMode::HIGH, SweepPressureMode::HIGH,
                   "high holds at half runway");
    ExpectPressure(39, 20, 100, false, SweepPressureMode::HIGH, SweepPressureMode::MEDIUM,
                   "high drops to medium below half runway");
    ExpectPressure(20, 20, 100, false, SweepPressureMode::MEDIUM, SweepPressureMode::MEDIUM,
                   "medium holds at quarter runway");
    ExpectPressure(19, 20, 100, false, SweepPressureMode::MEDIUM, SweepPressureMode::LOW,
                   "medium drops below quarter runway");

    // Empty backlog wins over stale/forced state; a non-empty zero-runway backlog is HIGH.
    ExpectPressure(0, 0, 100, true, SweepPressureMode::HIGH, SweepPressureMode::LOW,
                   "empty backlog always resolves low");
    ExpectPressure(1, 0, 100, true, SweepPressureMode::LOW, SweepPressureMode::HIGH,
                   "forced pressure overrides natural low");
    ExpectPressure(1, 0, 0, false, SweepPressureMode::LOW, SweepPressureMode::HIGH,
                   "zero threshold with backlog is high");
    ExpectPressure(1, 100, 100, false, SweepPressureMode::LOW, SweepPressureMode::HIGH,
                   "swept bytes at threshold leave zero runway");

    // R = 101 verifies ceiling boundaries without multiplication.
    ExpectPressure(50, 50, 151, false, SweepPressureMode::LOW, SweepPressureMode::LOW,
                   "odd runway stays low below ceil half");
    ExpectPressure(51, 50, 151, false, SweepPressureMode::LOW, SweepPressureMode::MEDIUM,
                   "odd runway enters medium at ceil half");
    ExpectPressure(51, 50, 151, false, SweepPressureMode::HIGH, SweepPressureMode::HIGH,
                   "odd runway high hold uses ceil half");
    ExpectPressure(26, 50, 151, false, SweepPressureMode::MEDIUM, SweepPressureMode::MEDIUM,
                   "odd runway medium hold uses ceil quarter");
    ExpectPressure(25, 50, 151, false, SweepPressureMode::MEDIUM, SweepPressureMode::LOW,
                   "odd runway exits medium below ceil quarter");

    constexpr size_t max = std::numeric_limits<size_t>::max();
    ExpectPressure(2, max - 3, max, false, SweepPressureMode::LOW, SweepPressureMode::MEDIUM,
                   "runway comparison near SIZE_MAX cannot overflow");

    // Step54 P128 snapshots must escalate to natural HIGH.
    ExpectPressure(5982846976ULL, 4679434240ULL, 7389194917ULL, false,
                   SweepPressureMode::LOW, SweepPressureMode::HIGH,
                   "cycle 11 backlog exceeds remaining runway");
    ExpectPressure(1694859264ULL, 4244373504ULL, 4511373720ULL, false,
                   SweepPressureMode::LOW, SweepPressureMode::HIGH,
                   "representative P128 backlog exceeds remaining runway");
}

void TestDirectAssistRunsOneWholeBatch()
{
    ExpectEqual(ComputeMSSweepAssistDirectRegions(0, 4), 0,
                "zero debt does not run direct Assist");
    ExpectEqual(ComputeMSSweepAssistDirectRegions(3, 4), 0,
                "sub-batch debt is retained");
    ExpectEqual(ComputeMSSweepAssistDirectRegions(4, 4), 4,
                "one full batch is repaid");
    ExpectEqual(ComputeMSSweepAssistDirectRegions(9, 4), 4,
                "only one batch runs per allocation");
    ExpectEqual(ComputeMSSweepAssistDirectRegions(3, 0), 1,
                "an internal zero batch preserves forward progress");
}

void TestDebtAccumulatesPaysAndSaturates()
{
    MSSweepAssistDebtState debt;
    debt.Accumulate(3, kMSSweepAssistRatioScale / 2);
    ExpectEqual(debt.debtScaled,
                kMSSweepAssistRatioScale + kMSSweepAssistRatioScale / 2,
                "fractional P debt accumulation");
    debt.AccountPaidRegions(1);
    ExpectEqual(debt.debtScaled, kMSSweepAssistRatioScale / 2,
                "whole-region debt payment preserves remainder");
    debt.AccountPaidRegions(2);
    ExpectEqual(debt.debtScaled, 0,
                "payment cannot underflow P debt");
    debt.debtScaled = std::numeric_limits<uint64_t>::max() - 1;
    debt.Accumulate(2, kMSSweepAssistRatioScale);
    ExpectEqual(debt.debtScaled, std::numeric_limits<uint64_t>::max(),
                "P debt saturation");
}

void TestDebtConsumesWholeSweepResult()
{
    MSSweepAssistDebtState debt;
    debt.debtScaled = 4 * kMSSweepAssistRatioScale;

    MSSweepAssistResult result;
    result.creditPaidRegions = 1;
    result.assistPaidRegions = 2;
    debt.AccountSweepResult(result);
    ExpectEqual(debt.debtScaled, kMSSweepAssistRatioScale,
                "credit and direct Sweep work must repay one combined debt");

    debt.debtScaled = kMSSweepAssistRatioScale / 2;
    result.sweepComplete = true;
    debt.AccountSweepResult(result);
    ExpectEqual(debt.debtScaled, 0,
                "completed Sweep epoch must discard fractional P debt");
}

void TestCreditIsClaimedOnlyOnceAndSaturates()
{
    MSSweepCreditPool pool;
    pool.Add(5);
    ExpectEqual(pool.Take(3), 3, "first credit claim");
    ExpectEqual(pool.Take(3), 2, "second credit claim");
    ExpectEqual(pool.Take(1), 0, "credit cannot be claimed twice");

    pool.Reset();
    pool.Add(std::numeric_limits<size_t>::max() - 1);
    pool.Add(10);
    ExpectEqual(pool.Take(std::numeric_limits<size_t>::max()),
                std::numeric_limits<size_t>::max(),
                "credit saturation");
}

void TestConcurrentCreditClaimsNeverExceedSupply()
{
    constexpr size_t kCredits = 10000;
    constexpr size_t kThreads = 8;
    MSSweepCreditPool pool;
    pool.Add(kCredits);
    std::atomic<size_t> claimed {0};
    std::vector<std::thread> workers;
    for (size_t i = 0; i < kThreads; ++i) {
        workers.emplace_back([&pool, &claimed]() {
            for (;;) {
                size_t got = pool.Take(7);
                if (got == 0) {
                    return;
                }
                claimed.fetch_add(got, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    ExpectEqual(claimed.load(std::memory_order_relaxed), kCredits,
                "concurrent claims equal supplied credit");
    ExpectEqual(pool.Take(1), 0, "concurrent credit pool drained");
}

void TestAssistResultSeparatesCreditAndDirectWork()
{
    MSSweepAssistResult result;
    result.creditPaidRegions = 2;
    result.assistPaidRegions = 3;
    ExpectEqual(result.TotalPaidRegions(), 5, "split Sweep payment total");
    ExpectTrue(!result.sweepComplete, "Sweep result defaults to incomplete");
}

} // namespace

int main()
{
    TestRatioKeepsFractionalRegionPrecision();
    TestPressureScalingSaturates();
    TestPressureUsesRemainingRunwayAndHysteresis();
    TestDirectAssistRunsOneWholeBatch();
    TestDebtAccumulatesPaysAndSaturates();
    TestDebtConsumesWholeSweepResult();
    TestCreditIsClaimedOnlyOnceAndSaturates();
    TestConcurrentCreditClaimsNeverExceedSupply();
    TestAssistResultSeparatesCreditAndDirectWork();
    return 0;
}
