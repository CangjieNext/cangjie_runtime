#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

#define MRT_USE_MARK_SWEEP
#include "src/Heap/MAllocator/MSSweepDebtPolicy.h"

namespace {

using MapleRuntime::AccumulateMSSweepDebtScaled;
using MapleRuntime::ComputeMSSweepAssistRatioScaled;
using MapleRuntime::ComputeMSSweepAssistDirectRegions;
using MapleRuntime::EvaluateSweepPressure;
using MapleRuntime::MSSweepAssistResult;
using MapleRuntime::MSSweepCreditPool;
using MapleRuntime::PayMSSweepDebtRegions;
using MapleRuntime::ScaleMSSweepAssistRatioScaled;
using MapleRuntime::SweepPressureMode;
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

void TestPressureUsesUnsweptSweptAndThresholdBytes()
{
    ExpectTrue(EvaluateSweepPressure(101, 0, 100, false) == SweepPressureMode::HIGH,
               "unswept above threshold is high pressure");
    ExpectTrue(EvaluateSweepPressure(100, 1, 100, false) == SweepPressureMode::MEDIUM,
               "retained total above threshold is medium pressure");
    ExpectTrue(EvaluateSweepPressure(40, 60, 100, false) == SweepPressureMode::LOW,
               "retained total equal to threshold remains low pressure");
    ExpectTrue(EvaluateSweepPressure(0, 101, 100, false) == SweepPressureMode::MEDIUM,
               "swept bytes alone can make natural pressure medium");
    ExpectTrue(EvaluateSweepPressure(0, 0, 100, true) == SweepPressureMode::HIGH,
               "forced pressure overrides natural low pressure");
    ExpectTrue(EvaluateSweepPressure(std::numeric_limits<size_t>::max() - 10, 20,
                                     std::numeric_limits<size_t>::max(), false) ==
                   SweepPressureMode::MEDIUM,
               "retained-byte comparison must not wrap");
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
    uint64_t debt = 0;
    debt = AccumulateMSSweepDebtScaled(debt, 3, kMSSweepAssistRatioScale / 2);
    ExpectEqual(debt, kMSSweepAssistRatioScale + kMSSweepAssistRatioScale / 2,
                "fractional P debt accumulation");
    ExpectEqual(PayMSSweepDebtRegions(debt, 1), kMSSweepAssistRatioScale / 2,
                "whole-region debt payment preserves remainder");
    ExpectEqual(PayMSSweepDebtRegions(debt, 2), 0,
                "payment cannot underflow P debt");
    ExpectEqual(AccumulateMSSweepDebtScaled(std::numeric_limits<uint64_t>::max() - 1,
                                            2, kMSSweepAssistRatioScale),
                std::numeric_limits<uint64_t>::max(),
                "P debt saturation");
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
    ExpectEqual(pool.Load(), std::numeric_limits<size_t>::max(),
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
    ExpectEqual(pool.Load(), 0, "concurrent credit pool drained");
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
    TestPressureUsesUnsweptSweptAndThresholdBytes();
    TestDirectAssistRunsOneWholeBatch();
    TestDebtAccumulatesPaysAndSaturates();
    TestCreditIsClaimedOnlyOnceAndSaturates();
    TestConcurrentCreditClaimsNeverExceedSupply();
    TestAssistResultSeparatesCreditAndDirectWork();
    return 0;
}
