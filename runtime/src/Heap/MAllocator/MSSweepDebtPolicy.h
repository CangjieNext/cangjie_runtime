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

}  // namespace MapleRuntime

#endif  // MRT_USE_MARK_SWEEP
#endif  // MRT_MS_SWEEP_DEBT_POLICY_H
