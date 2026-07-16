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
// Sweep-debt policy (Step34 §14-19): backlog-aware HEU gating + tiered pressure.
//
// Pure, header-only computation so the waterline / pressure-mode logic can be
// reasoned about (and unit-tested) without dragging in MSCentralRegionPool.
//
// Concepts:
//   runway   = how much more allocation can happen before the next HEU trigger,
//              measured on the swept-region-bytes axis (threshold - sweptBytes).
//   waterline= hysteretic enter/exit thresholds so the mode does not flap at the
//              boundary (BACKLOG enter ratio > exit ratio; EMERGENCY enter > exit).
//   pressure = NORMAL / BACKLOG / EMERGENCY, scaling how hard background + assist
//              sweep repays debt. Semantics: "how far sweep debt has fallen behind
//              the runway we still allow allocation against" — not "hardPressure".
// ============================================================================
namespace MapleRuntime {

enum class SweepPressureMode : int {
    NORMAL = 0,
    BACKLOG = 1,
    EMERGENCY = 2,
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
        case SweepPressureMode::BACKLOG:
            multiplier = 4;
            break;
        case SweepPressureMode::EMERGENCY:
            multiplier = 10;
            break;
        case SweepPressureMode::NORMAL:
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

struct SweepDebtParams {
    // BACKLOG enters when unswept > enter*runway, exits when unswept <= exit*runway.
    double backlogEnterRatio = 2.0;
    double backlogExitRatio = 1.0;
    // EMERGENCY (runway == 0) enters above enter regions, exits below exit regions.
    size_t emergencyEnterRegions = 64;
    size_t emergencyExitRegions = 16;
};

struct SweepDebtDecision {
    SweepPressureMode mode = SweepPressureMode::NORMAL;
    bool deferHeu = false;                  // defer this GC_REASON_HEU, repay first
    double assistRatioMultiplier = 1.0;     // scale sweepRegionsPerAlloc_
    double assistCapMultiplier = 1.0;       // scale msAssistSweepMaxRegionsPerCall
    double backgroundBudgetMultiplier = 1.0; // scale per-slice background time budget
};

enum class MSHeuExecutionDecision : uint8_t {
    RUN = 0,
    BELOW_THRESHOLD,
    DEFER_FOR_SWEEP,
};

// Revalidate an asynchronous HEU request against the current heap state. The
// request may have waited behind another GC, so the state that enqueued it is
// not authoritative when the GC thread eventually dequeues it.
inline MSHeuExecutionDecision EvaluateMSHeuExecution(size_t sweptBytes, size_t thresholdBytes,
                                                     const SweepDebtDecision& debt)
{
    if (sweptBytes < thresholdBytes) {
        return MSHeuExecutionDecision::BELOW_THRESHOLD;
    }
    if (debt.deferHeu) {
        return MSHeuExecutionDecision::DEFER_FOR_SWEEP;
    }
    return MSHeuExecutionDecision::RUN;
}

// runway in regions on the swept-bytes axis the HEU trigger already uses.
inline size_t ComputeRunwayRegions(size_t thresholdBytes, size_t sweptBytes, size_t regionSize)
{
    if (regionSize == 0) {
        return 0;
    }
    size_t runwayBytes = thresholdBytes > sweptBytes ? (thresholdBytes - sweptBytes) : 0;
    return (runwayBytes + regionSize - 1) / regionSize;  // ceil
}

// Evaluate the pressure mode with hysteresis. prev is the previously latched mode.
inline SweepPressureMode EvaluateSweepPressure(size_t unsweptRegions, size_t runwayRegions,
                                               bool sweepDone, SweepPressureMode prev,
                                               const SweepDebtParams& p)
{
    if (sweepDone || unsweptRegions == 0) {
        return SweepPressureMode::NORMAL;
    }
    if (runwayRegions == 0) {
        // Runway exhausted: "above runway" loses meaning, gate purely on backlog size.
        bool wasEmergency = (prev == SweepPressureMode::EMERGENCY);
        size_t threshold = wasEmergency ? p.emergencyExitRegions : p.emergencyEnterRegions;
        return unsweptRegions > threshold ? SweepPressureMode::EMERGENCY : SweepPressureMode::NORMAL;
    }
    bool wasBacklog = (prev != SweepPressureMode::NORMAL);
    double ratio = wasBacklog ? p.backlogExitRatio : p.backlogEnterRatio;
    double threshold = ratio * static_cast<double>(runwayRegions);
    return static_cast<double>(unsweptRegions) > threshold ? SweepPressureMode::BACKLOG
                                                           : SweepPressureMode::NORMAL;
}

// Tier multipliers: NORMAL=1x, BACKLOG=4x, EMERGENCY=10x.
inline double SweepTierMultiplier(SweepPressureMode mode)
{
    switch (mode) {
        case SweepPressureMode::BACKLOG: return 4.0;
        case SweepPressureMode::EMERGENCY: return 10.0;
        case SweepPressureMode::NORMAL:
        default: return 1.0;
    }
}

inline SweepDebtDecision BuildSweepDebtDecision(SweepPressureMode mode, bool sweepDone)
{
    SweepDebtDecision d;
    d.mode = mode;
    double m = SweepTierMultiplier(mode);
    d.assistRatioMultiplier = m;
    d.assistCapMultiplier = m;
    d.backgroundBudgetMultiplier = m;
    // Only BACKLOG/EMERGENCY with outstanding debt defer the next HEU GC. At the
    // HEU gate runway is ~0, so this resolves to EMERGENCY in practice; the BACKLOG
    // tier mainly drives the assist/background multipliers away from the gate.
    d.deferHeu = (mode != SweepPressureMode::NORMAL) && !sweepDone;
    return d;
}

}  // namespace MapleRuntime

#endif  // MRT_USE_MARK_SWEEP
#endif  // MRT_MS_SWEEP_DEBT_POLICY_H
