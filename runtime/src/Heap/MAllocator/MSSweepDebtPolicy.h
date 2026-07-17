// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_SWEEP_DEBT_POLICY_H
#define MRT_MS_SWEEP_DEBT_POLICY_H

#ifdef MRT_USE_MARK_SWEEP

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

// ============================================================================
// Sweep-debt policy: P-local pacing plus one unified U/S/T pressure state.
//
// Pure, header-only computation so the waterline / pressure-mode logic can be
// reasoned about (and unit-tested) without dragging in MSCentralRegionPool.
//
// Pressure is LOW / MEDIUM / HIGH. It scales debt addition and time-bounded
// Background/Helper slices, but never changes one direct P Assist batch.
// ============================================================================
namespace MapleRuntime {

enum class SweepPressureMode : int {
    LOW = 0,
    MEDIUM = 1,
    HIGH = 2,
};

// Sweep debt is accounted in fixed-point regions.  The fractional part lets a
// P accumulate a ratio below one region/allocation without falling back to a
// process-wide allocation counter.
constexpr uint64_t kMSSweepAssistRatioScale = uint64_t {1} << 16;

inline uint64_t SaturatingAddMSSweepValue(uint64_t lhs, uint64_t rhs)
{
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    return rhs > max - lhs ? max : lhs + rhs;
}

inline uint64_t SaturatingMultiplyMSSweepValue(uint64_t lhs, uint64_t rhs)
{
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    return lhs > max / rhs ? max : lhs * rhs;
}

// Compute floor(remainder * 2^16 / denominator) without overflowing a
// uint64_t.  At each bit, remainder is kept strictly below denominator.
inline uint64_t ComputeMSSweepFractionScaled(uint64_t remainder, uint64_t denominator)
{
    uint64_t result = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        result <<= 1;
        if (remainder >= denominator - remainder) {
            remainder -= denominator - remainder;
            ++result;
        } else {
            remainder += remainder;
        }
    }
    return result;
}

inline uint64_t ComputeMSSweepAssistRatioScaled(size_t totalUnsweptRegions, size_t runwayRegions)
{
    if (totalUnsweptRegions == 0) {
        return 0;
    }
    const uint64_t numerator = static_cast<uint64_t>(totalUnsweptRegions);
    const uint64_t denominator = static_cast<uint64_t>(std::max<size_t>(runwayRegions, 1));
    const uint64_t whole = numerator / denominator;
    const uint64_t base = SaturatingMultiplyMSSweepValue(whole, kMSSweepAssistRatioScale);
    if (base == std::numeric_limits<uint64_t>::max()) {
        return base;
    }
    const uint64_t fraction = ComputeMSSweepFractionScaled(numerator % denominator, denominator);
    return SaturatingAddMSSweepValue(base, fraction);
}

inline uint64_t ScaleMSSweepAssistRatioScaled(uint64_t ratioScaled, SweepPressureMode mode)
{
    uint64_t multiplier = 1;
    switch (mode) {
        case SweepPressureMode::MEDIUM:
            multiplier = 4;
            break;
        case SweepPressureMode::HIGH:
            multiplier = 10;
            break;
        case SweepPressureMode::LOW:
        default:
            break;
    }
    return SaturatingMultiplyMSSweepValue(ratioScaled, multiplier);
}

inline uint64_t AccumulateMSSweepDebtScaled(uint64_t debtScaled, size_t allocatedRegions,
                                            uint64_t ratioScaled)
{
    uint64_t added = SaturatingMultiplyMSSweepValue(static_cast<uint64_t>(allocatedRegions), ratioScaled);
    return SaturatingAddMSSweepValue(debtScaled, added);
}

inline uint64_t PayMSSweepDebtRegions(uint64_t debtScaled, size_t paidRegions)
{
    uint64_t paidScaled = SaturatingMultiplyMSSweepValue(static_cast<uint64_t>(paidRegions),
                                                         kMSSweepAssistRatioScale);
    return paidScaled >= debtScaled ? 0 : debtScaled - paidScaled;
}

// Direct repayment is deliberately batched: retain a sub-batch remainder and,
// once the threshold is reached, run exactly one batch in this allocation call.
inline size_t ComputeMSSweepAssistDirectRegions(size_t unpaidDebtRegions,
                                                 size_t configuredBatchRegions)
{
    size_t batchRegions = std::max<size_t>(configuredBatchRegions, 1);
    return unpaidDebtRegions < batchRegions ? 0 : batchRegions;
}

inline bool ResetMSSweepDebtForGeneration(uint64_t& debtScaled, uint32_t& generation,
                                          uint32_t currentGeneration)
{
    if (generation == currentGeneration) {
        return false;
    }
    debtScaled = 0;
    generation = currentGeneration;
    return true;
}

enum class MSSweepWorkSource : uint8_t {
    BACKGROUND = 0,
    LAZY,
    ASSIST,
    HELPER,
    FORCE_FINISH,
};

inline bool ShouldPublishMSSweepCredit(MSSweepWorkSource source)
{
    return source == MSSweepWorkSource::BACKGROUND || source == MSSweepWorkSource::LAZY ||
           source == MSSweepWorkSource::HELPER;
}

// Work completed outside the allocating P is published once and claimed with
// CAS.  Assist work does not enter this pool because it repays its owner P
// directly; this prevents one swept Region from being counted twice.
class MSSweepCreditPool {
public:
    void Reset()
    {
        credits_.store(0, std::memory_order_release);
    }

    void Add(size_t regions)
    {
        size_t current = credits_.load(std::memory_order_relaxed);
        for (;;) {
            constexpr size_t max = std::numeric_limits<size_t>::max();
            size_t desired = regions > max - current ? max : current + regions;
            if (credits_.compare_exchange_weak(current, desired, std::memory_order_release,
                                               std::memory_order_relaxed)) {
                return;
            }
        }
    }

    size_t Take(size_t requested)
    {
        size_t current = credits_.load(std::memory_order_acquire);
        while (current != 0 && requested != 0) {
            size_t claimed = std::min(current, requested);
            if (credits_.compare_exchange_weak(current, current - claimed, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                return claimed;
            }
        }
        return 0;
    }

    size_t Load() const
    {
        return credits_.load(std::memory_order_acquire);
    }

private:
    std::atomic<size_t> credits_ {0};
};

struct MSSweepAssistResult {
    size_t creditPaidRegions = 0;
    size_t assistPaidRegions = 0;
    bool sweepComplete = false;

    size_t TotalPaidRegions() const
    {
        constexpr size_t max = std::numeric_limits<size_t>::max();
        return assistPaidRegions > max - creditPaidRegions ? max
                                                           : creditPaidRegions + assistPaidRegions;
    }
};

struct MSSweepAssistStats {
    uint64_t calls = 0;
    uint64_t debtAddedScaled = 0;
    uint64_t creditPaidRegions = 0;
    uint64_t assistPaidRegions = 0;
    uint64_t maxDebtScaled = 0;

    void Merge(const MSSweepAssistStats& other)
    {
        calls = SaturatingAddMSSweepValue(calls, other.calls);
        debtAddedScaled = SaturatingAddMSSweepValue(debtAddedScaled, other.debtAddedScaled);
        creditPaidRegions = SaturatingAddMSSweepValue(creditPaidRegions, other.creditPaidRegions);
        assistPaidRegions = SaturatingAddMSSweepValue(assistPaidRegions, other.assistPaidRegions);
        maxDebtScaled = std::max(maxDebtScaled, other.maxDebtScaled);
    }

    MSSweepAssistStats TakeAndReset()
    {
        MSSweepAssistStats snapshot = *this;
        *this = MSSweepAssistStats {};
        return snapshot;
    }
};

inline void SaturatingAddMSSweepAtomic(std::atomic<uint64_t>& value, uint64_t delta)
{
    uint64_t current = value.load(std::memory_order_relaxed);
    for (;;) {
        uint64_t desired = SaturatingAddMSSweepValue(current, delta);
        if (value.compare_exchange_weak(current, desired, std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
            return;
        }
    }
}

// StopAndWaitForAllSweep emits its closing-cycle REPORT before the following
// STW begins.  These atomics let that thread snapshot active P statistics while
// the owning P may still finish an allocation-side update.
class MSSweepAssistAtomicStats {
public:
    void RecordDebt(uint64_t addedScaled, uint64_t currentDebtScaled)
    {
        SaturatingAddMSSweepAtomic(calls_, 1);
        SaturatingAddMSSweepAtomic(debtAddedScaled_, addedScaled);
        uint64_t currentMax = maxDebtScaled_.load(std::memory_order_relaxed);
        while (currentMax < currentDebtScaled &&
               !maxDebtScaled_.compare_exchange_weak(currentMax, currentDebtScaled,
                                                      std::memory_order_relaxed,
                                                      std::memory_order_relaxed)) {
        }
    }

    void RecordPayment(size_t creditPaidRegions, size_t assistPaidRegions)
    {
        SaturatingAddMSSweepAtomic(creditPaidRegions_, static_cast<uint64_t>(creditPaidRegions));
        SaturatingAddMSSweepAtomic(assistPaidRegions_, static_cast<uint64_t>(assistPaidRegions));
    }

    MSSweepAssistStats Snapshot() const
    {
        MSSweepAssistStats snapshot;
        snapshot.calls = calls_.load(std::memory_order_relaxed);
        snapshot.debtAddedScaled = debtAddedScaled_.load(std::memory_order_relaxed);
        snapshot.creditPaidRegions = creditPaidRegions_.load(std::memory_order_relaxed);
        snapshot.assistPaidRegions = assistPaidRegions_.load(std::memory_order_relaxed);
        snapshot.maxDebtScaled = maxDebtScaled_.load(std::memory_order_relaxed);
        return snapshot;
    }

    void Reset()
    {
        calls_.store(0, std::memory_order_relaxed);
        debtAddedScaled_.store(0, std::memory_order_relaxed);
        creditPaidRegions_.store(0, std::memory_order_relaxed);
        assistPaidRegions_.store(0, std::memory_order_relaxed);
        maxDebtScaled_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> calls_ {0};
    std::atomic<uint64_t> debtAddedScaled_ {0};
    std::atomic<uint64_t> creditPaidRegions_ {0};
    std::atomic<uint64_t> assistPaidRegions_ {0};
    std::atomic<uint64_t> maxDebtScaled_ {0};
};

struct SweepDebtDecision {
    SweepPressureMode mode = SweepPressureMode::LOW;
    double assistRatioMultiplier = 1.0;     // scale sweepRegionsPerAlloc_
    double backgroundBudgetMultiplier = 1.0; // scale per-slice background time budget
};

// runway in regions on the swept-bytes axis the HEU trigger already uses.
inline size_t ComputeRunwayRegions(size_t thresholdBytes, size_t sweptBytes, size_t regionSize)
{
    if (regionSize == 0) {
        return 0;
    }
    size_t runwayBytes = thresholdBytes > sweptBytes ? (thresholdBytes - sweptBytes) : 0;
    return (runwayBytes + regionSize - 1) / regionSize;  // ceil
}

inline bool MSSweepBytesExceedThreshold(size_t lhs, size_t rhs, size_t threshold)
{
    if (lhs > threshold || rhs > threshold) {
        return true;
    }
    return lhs > threshold - rhs;
}

// U is unswept small-region bytes, S is swept small-region plus live large-span
// bytes, and T is the current heap threshold. The sum comparison is written
// without addition so counters near SIZE_MAX cannot wrap.
inline SweepPressureMode EvaluateSweepPressure(size_t unsweptBytes, size_t sweptBytes,
                                               size_t thresholdBytes, bool forcedHigh)
{
    if (forcedHigh || unsweptBytes > thresholdBytes) {
        return SweepPressureMode::HIGH;
    }
    if (MSSweepBytesExceedThreshold(sweptBytes, unsweptBytes, thresholdBytes)) {
        return SweepPressureMode::MEDIUM;
    }
    return SweepPressureMode::LOW;
}

// Tier multipliers: LOW=1x, MEDIUM=4x, HIGH=10x.
inline size_t SweepTierMultiplier(SweepPressureMode mode)
{
    switch (mode) {
        case SweepPressureMode::MEDIUM: return 4;
        case SweepPressureMode::HIGH: return 10;
        case SweepPressureMode::LOW:
        default: return 1;
    }
}

inline SweepDebtDecision BuildSweepDebtDecision(SweepPressureMode mode)
{
    SweepDebtDecision d;
    d.mode = mode;
    double m = static_cast<double>(SweepTierMultiplier(mode));
    d.assistRatioMultiplier = m;
    d.backgroundBudgetMultiplier = m;
    return d;
}

}  // namespace MapleRuntime

#endif  // MRT_USE_MARK_SWEEP
#endif  // MRT_MS_SWEEP_DEBT_POLICY_H
