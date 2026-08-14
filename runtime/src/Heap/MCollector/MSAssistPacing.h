// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_ASSIST_PACING_H
#define MRT_MS_ASSIST_PACING_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "Heap/MAllocator/MSSweepPressurePolicy.h"
#include "Heap/MCollector/MSWorkUnit.h"

namespace MapleRuntime {

constexpr uint64_t kMSAssistRatioScale = 65536;

// Mark-assist pacing parameters. Debt is tracked in Q16 WorkUnit units so
// allocation-sized charges preserve the fractional remainder between refills.
constexpr uint64_t MS_WU_ASSIST_THRESHOLD = 2048;
constexpr uint64_t MS_WU_ASSIST_WALL_BUDGET_US = 100;
constexpr uint64_t MS_MARK_WORK_WORDS_PER_PARTICIPANT = 128;
constexpr uint8_t MS_MARK_ASSIST_UP_WINDOWS = 2;
constexpr uint8_t MS_MARK_ASSIST_DOWN_WINDOWS = 2;

inline uint64_t SaturatingMSWUMul(uint64_t lhs, uint64_t rhs)
{
    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    return lhs > std::numeric_limits<uint64_t>::max() / rhs
        ? std::numeric_limits<uint64_t>::max()
        : lhs * rhs;
}

// Mark pacing uses Q16 WorkUnit density so one completed WorkUnit per byte is
// represented exactly while P-local allocation debt retains fractional work.
constexpr uint64_t MS_WU_Q16_ONE = UINT64_C(1) << 16;
constexpr uint64_t MS_WU_INITIAL_DENSITY_Q16 = MS_WU_Q16_ONE / 64;

// Scheduler-control sampling and worker-width pacing are part of the same
// feedback loop as the mutator-assist multiplier. Generic WorkUnit accounting
// itself remains in MSWorkUnit.h.
constexpr size_t MS_MARK_CONTROL_SLICES_PER_UPDATE = 4;
constexpr uint8_t MS_MARK_HELPER_UP_WINDOWS = 4;
constexpr uint8_t MS_MARK_HELPER_DOWN_WINDOWS = 3;
constexpr size_t MS_MARK_NORMAL_INITIAL_HELPERS = 2;
constexpr uint64_t MS_MARK_MIN_TREND_DEADBAND_WORDS = 32;

inline uint64_t MSWUQ16Divide(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0) {
        return std::numeric_limits<uint64_t>::max();
    }
    __uint128_t scaled = static_cast<__uint128_t>(numerator) << 16;
    __uint128_t quotient = scaled / denominator;
    return quotient > std::numeric_limits<uint64_t>::max()
        ? std::numeric_limits<uint64_t>::max()
        : static_cast<uint64_t>(quotient);
}

inline uint64_t UpdateMSWUEwmaOneEighth(uint64_t previous, uint64_t sample)
{
    if (sample >= previous) {
        return SaturatingMSWUAdd(previous, (sample - previous) / 8);
    }
    return previous - (previous - sample) / 8;
}

enum class MSMarkPressure : uint8_t {
    NORMAL,
    HIGH,
    CRITICAL,
};

// A long-running Mark cycle allows mutators to build a large outside-set even
// when the heap is not close to a configured limit. Use elapsed time as an
// early pressure signal, but only while there is unfinished mark work.
constexpr uint64_t MS_MARK_HIGH_ELAPSED_US = 20 * 1000;
constexpr uint64_t MS_MARK_CRITICAL_ELAPSED_US = 100 * 1000;
constexpr uint8_t MS_MARK_HIGH_PRESSURE_WINDOWS = 3;
constexpr uint8_t MS_MARK_CRITICAL_PRESSURE_WINDOWS = 2;

inline MSMarkPressure ClassifyMSMarkElapsedPressure(
    uint64_t elapsedMarkUs, bool hasOutstandingWork)
{
    if (!hasOutstandingWork || elapsedMarkUs < MS_MARK_HIGH_ELAPSED_US) {
        return MSMarkPressure::NORMAL;
    }
    return elapsedMarkUs >= MS_MARK_CRITICAL_ELAPSED_US
        ? MSMarkPressure::CRITICAL
        : MSMarkPressure::HIGH;
}

inline size_t ComputeMSMarkHelperTarget(
    uint64_t outstandingTasks, uint64_t outstandingWorkWords,
    size_t maxHelpers, MSMarkPressure pressure)
{
    if (outstandingTasks == 0 || outstandingWorkWords == 0 ||
        maxHelpers == 0) {
        return 0;
    }
    // GC main owns the first work share. Pending bitmap words (plus one unit
    // per Object task) estimate the remaining scan work; task count only caps
    // how many participants can claim independent work.
    uint64_t participants = 1 +
        (outstandingWorkWords - 1) /
            MS_MARK_WORK_WORDS_PER_PARTICIPANT;
    uint64_t helperWorkTarget = participants > 1 ? participants - 1 : 0;
    size_t target = static_cast<size_t>(std::min<uint64_t>(
        helperWorkTarget, static_cast<uint64_t>(maxHelpers)));
    if (pressure == MSMarkPressure::HIGH) {
        target = std::max(target, (maxHelpers + 1) / 2);
    } else if (pressure == MSMarkPressure::CRITICAL) {
        target = maxHelpers;
    }
    uint64_t taskHelpers = pressure == MSMarkPressure::NORMAL
        ? outstandingTasks - 1
        : outstandingTasks;
    size_t outstandingLimit = static_cast<size_t>(std::min<uint64_t>(
        taskHelpers, std::numeric_limits<size_t>::max()));
    return std::min(target, std::min(maxHelpers, outstandingLimit));
}

inline uint64_t UpdateMSMarkWorkWordsEwma(
    uint64_t previous, uint64_t sample)
{
    if (sample >= previous) {
        return previous + (sample - previous) / 4;
    }
    return previous - (previous - sample) / 4;
}

inline size_t ComputeMSMarkInitialAssistMultiplier(
    size_t maxMultiplier, MSMarkPressure pressure)
{
    maxMultiplier = std::max<size_t>(1, maxMultiplier);
    if (pressure == MSMarkPressure::CRITICAL) {
        return maxMultiplier;
    }
    return pressure == MSMarkPressure::HIGH
        ? (maxMultiplier + 1) / 2
        : 1;
}

inline size_t ComputeMSMarkInitialHelperTarget(
    size_t desiredHelpers, size_t maxHelpers, MSMarkPressure pressure)
{
    desiredHelpers = std::min(desiredHelpers, maxHelpers);
    if (pressure == MSMarkPressure::CRITICAL) {
        return desiredHelpers;
    }
    size_t initialCap = pressure == MSMarkPressure::HIGH
        ? std::max<size_t>(MS_MARK_NORMAL_INITIAL_HELPERS,
                           (maxHelpers + 1) / 2)
        : MS_MARK_NORMAL_INITIAL_HELPERS;
    return std::min(desiredHelpers, initialCap);
}

// The controller jointly paces GC helpers and mutator assist from the same
// backlog sample. It is policy state, not scheduler state, so it lives with
// the rest of assist pacing rather than WorkUnit accounting.
class MSMarkControllerState {
public:
    MSMarkControllerState(
        uint64_t outstandingTasks, uint64_t outstandingWorkWords,
        size_t maxHelpers, size_t maxAssistMultiplier,
        MSMarkPressure pressure)
        : maxHelpers_(maxHelpers),
          maxAssistMultiplier_(std::max<size_t>(1, maxAssistMultiplier)),
          pressure_(pressure),
          acceptedPressure_(pressure),
          filteredWorkWords_(outstandingWorkWords),
          helperTarget_(ComputeMSMarkInitialHelperTarget(
              ComputeMSMarkHelperTarget(
                  outstandingTasks, outstandingWorkWords,
                  maxHelpers, pressure),
              maxHelpers, pressure)),
          minAssistMultiplier_(ComputeMSMarkInitialAssistMultiplier(
              maxAssistMultiplier_, pressure)),
          assistMultiplier_(minAssistMultiplier_)
    {
    }

    // A pressure level is deliberately latched for the current Mark cycle:
    // elapsed time never moves backwards, and lowering helper/assist width on
    // a transient tail would only add scheduler churn. NORMAL samples still
    // reset incomplete escalation streaks before the latch is raised.
    void ObserveSchedulerSample(uint64_t outstandingTasks,
                                uint64_t outstandingWorkWords,
                                uint64_t elapsedMarkUs)
    {
        MSMarkPressure observedPressure = ClassifyMSMarkElapsedPressure(
            elapsedMarkUs, outstandingTasks != 0 || outstandingWorkWords != 0);
        EscalatePressure(ObservePressure(observedPressure),
                         outstandingTasks, outstandingWorkWords);

        uint64_t previousFilteredWorkWords = filteredWorkWords_;
        filteredWorkWords_ = UpdateMSMarkWorkWordsEwma(
            filteredWorkWords_, outstandingWorkWords);

        size_t desiredHelpers = ComputeMSMarkHelperTarget(
            outstandingTasks, filteredWorkWords_,
            maxHelpers_, pressure_);
        UpdateHelpers(desiredHelpers);
        UpdateAssist(previousFilteredWorkWords);
    }

private:
    void EscalatePressure(MSMarkPressure pressure, uint64_t outstandingTasks,
                          uint64_t outstandingWorkWords)
    {
        if (static_cast<uint8_t>(pressure) <= static_cast<uint8_t>(pressure_)) {
            return;
        }
        pressure_ = pressure;
        filteredWorkWords_ = std::max(filteredWorkWords_, outstandingWorkWords);
        helperTarget_ = ComputeMSMarkHelperTarget(
            outstandingTasks, filteredWorkWords_, maxHelpers_, pressure_);
        minAssistMultiplier_ = ComputeMSMarkInitialAssistMultiplier(
            maxAssistMultiplier_, pressure_);
        assistMultiplier_ = std::max(assistMultiplier_, minAssistMultiplier_);
        helperUpWindows_ = 0;
        helperDownWindows_ = 0;
        helperStableWindows_ = 0;
        assistUpWindows_ = 0;
        assistDownWindows_ = 0;
    }

public:
    size_t HelperTarget() const
    {
        return helperTarget_;
    }

    size_t AssistMultiplier() const
    {
        return assistMultiplier_;
    }

    MSMarkPressure Pressure() const
    {
        return pressure_;
    }

private:
    MSMarkPressure ObservePressure(MSMarkPressure observedPressure)
    {
        if (acceptedPressure_ == MSMarkPressure::CRITICAL) {
            return acceptedPressure_;
        }
        if (observedPressure == MSMarkPressure::NORMAL) {
            highPressureWindows_ = 0;
            criticalPressureWindows_ = 0;
            return acceptedPressure_;
        }

        ++highPressureWindows_;
        if (observedPressure == MSMarkPressure::CRITICAL) {
            ++criticalPressureWindows_;
            if (criticalPressureWindows_ >= MS_MARK_CRITICAL_PRESSURE_WINDOWS) {
                acceptedPressure_ = MSMarkPressure::CRITICAL;
                return acceptedPressure_;
            }
        } else {
            criticalPressureWindows_ = 0;
        }

        if (acceptedPressure_ == MSMarkPressure::NORMAL &&
            highPressureWindows_ >= MS_MARK_HIGH_PRESSURE_WINDOWS) {
            acceptedPressure_ = MSMarkPressure::HIGH;
        }
        return acceptedPressure_;
    }

    void UpdateHelpers(size_t desiredHelpers)
    {
        if (desiredHelpers > helperTarget_) {
            helperDownWindows_ = 0;
            helperStableWindows_ = 0;
            if (++helperUpWindows_ >= MS_MARK_HELPER_UP_WINDOWS) {
                ++helperTarget_;
                helperUpWindows_ = 0;
            }
            return;
        }
        if (desiredHelpers < helperTarget_) {
            helperUpWindows_ = 0;
            helperStableWindows_ = 0;
            if (++helperDownWindows_ >= MS_MARK_HELPER_DOWN_WINDOWS) {
                --helperTarget_;
                helperDownWindows_ = 0;
            }
            return;
        }
        helperUpWindows_ = 0;
        helperDownWindows_ = 0;
        if (helperStableWindows_ != std::numeric_limits<uint8_t>::max()) {
            ++helperStableWindows_;
        }
    }

    void UpdateAssist(uint64_t previousFilteredWorkWords)
    {
        if (pressure_ == MSMarkPressure::CRITICAL) {
            assistMultiplier_ = maxAssistMultiplier_;
            return;
        }

        uint64_t deadband = std::max(
            MS_MARK_MIN_TREND_DEADBAND_WORDS,
            previousFilteredWorkWords / 8);
        bool draining = previousFilteredWorkWords > filteredWorkWords_ &&
            previousFilteredWorkWords - filteredWorkWords_ > deadband;
        uint64_t participantCount = static_cast<uint64_t>(helperTarget_) + 1;
        uint64_t lowWater = SaturatingMSWUMul(
            participantCount, MS_MARK_WORK_WORDS_PER_PARTICIPANT);
        uint64_t highWater = SaturatingMSWUMul(lowWater, 2);

        bool canRaise = assistMultiplier_ < maxAssistMultiplier_ &&
            helperStableWindows_ != 0 &&
            filteredWorkWords_ >= highWater && !draining;
        if (canRaise) {
            assistDownWindows_ = 0;
            if (++assistUpWindows_ >= MS_MARK_ASSIST_UP_WINDOWS) {
                ++assistMultiplier_;
                assistUpWindows_ = 0;
            }
            return;
        }

        assistUpWindows_ = 0;
        bool shouldLower = assistMultiplier_ > minAssistMultiplier_ &&
            (draining || filteredWorkWords_ <= lowWater);
        if (shouldLower) {
            if (++assistDownWindows_ >= MS_MARK_ASSIST_DOWN_WINDOWS) {
                --assistMultiplier_;
                assistDownWindows_ = 0;
            }
        } else {
            assistDownWindows_ = 0;
        }
    }

    size_t maxHelpers_ = 0;
    size_t maxAssistMultiplier_ = 1;
    MSMarkPressure pressure_ = MSMarkPressure::NORMAL;
    MSMarkPressure acceptedPressure_ = MSMarkPressure::NORMAL;
    uint64_t filteredWorkWords_ = 0;
    size_t helperTarget_ = 0;
    size_t minAssistMultiplier_ = 1;
    size_t assistMultiplier_ = 1;
    uint8_t helperUpWindows_ = 0;
    uint8_t helperDownWindows_ = 0;
    uint8_t helperStableWindows_ = 0;
    uint8_t assistUpWindows_ = 0;
    uint8_t assistDownWindows_ = 0;
    uint8_t highPressureWindows_ = 0;
    uint8_t criticalPressureWindows_ = 0;
};

// The completed-cycle plan always starts at normal pressure. Higher pressure
// is applied at activation through its multiplier, so the persisted base rate
// only needs density and the runtime disable gate.
inline uint64_t ResolveMSMarkBaseAssistRatioQ16(
    uint64_t workDensityQ16, bool assistDisabled)
{
    return assistDisabled ? 0 : workDensityQ16 / 4;
}

struct MSMarkAssistPlan {
    uint64_t baseRatio = 0;
    size_t maxMultiplier = 1;
};

// Owns the cross-cycle Mark-assist feedback state. The collector supplies
// measured work and normalized RuntimeParam values, while this object derives
// the next plan and the current cycle's published allocation charge rate.
class MSMarkAssistPacing {
public:
    void UpdateForCompletedCycle(uint64_t completedWU, uint64_t allocatedBytes,
                                 size_t configuredMaxMultiplier,
                                 bool assistDisabled)
    {
        if (allocatedBytes != 0) {
            workDensityQ16_ = UpdateMSWUEwmaOneEighth(
                workDensityQ16_, MSWUQ16Divide(completedWU, allocatedBytes));
        }
        nextPlan_.baseRatio = ResolveMSMarkBaseAssistRatioQ16(
            workDensityQ16_, assistDisabled);
        nextPlan_.maxMultiplier = std::max<size_t>(
            1, configuredMaxMultiplier);
    }

    MSMarkAssistPlan ActivateForCurrentCycle(
        MSMarkPressure pressure)
    {
        size_t multiplier = ComputeMSMarkInitialAssistMultiplier(
            nextPlan_.maxMultiplier, pressure);
        PublishRateForMultiplier(nextPlan_, multiplier);
        return nextPlan_;
    }

    void PublishRateForMultiplier(const MSMarkAssistPlan& plan,
                                  size_t multiplier)
    {
        uint64_t rateScaled = SaturatingMSWUMul(plan.baseRatio, multiplier);
        if (publishedRateScaled_.load(std::memory_order_acquire) != rateScaled) {
            publishedRateScaled_.store(rateScaled, std::memory_order_release);
        }
    }

    uint64_t GetPublishedRateScaled() const
    {
        return publishedRateScaled_.load(std::memory_order_acquire);
    }

    void DeactivateCurrentCycle()
    {
        publishedRateScaled_.store(0, std::memory_order_release);
    }

    void ResetCurrentCycle()
    {
        DeactivateCurrentCycle();
        completedAssistWU_.store(0, std::memory_order_relaxed);
    }

    void AccountCompletedAssistWork(uint64_t completedWU)
    {
        if (completedWU != 0) {
            completedAssistWU_.fetch_add(
                completedWU, std::memory_order_relaxed);
        }
    }

    uint64_t DrainCompletedAssistWork()
    {
        return completedAssistWU_.exchange(0, std::memory_order_relaxed);
    }

private:
    // Cross-cycle density EWMA is an implementation detail of this pacing
    // owner. Callers receive only an immutable plan and published rate.
    uint64_t workDensityQ16_ = MS_WU_INITIAL_DENSITY_Q16;
    MSMarkAssistPlan nextPlan_;
    std::atomic<uint64_t> publishedRateScaled_ {0};
    std::atomic<uint64_t> completedAssistWU_ {0};
};

inline uint64_t ComputeMSMarkAssistSliceBudget(uint64_t debtWU)
{
    return std::max<uint64_t>(MS_WU_ASSIST_THRESHOLD, debtWU);
}

// Mutator assist is useful only when there is enough globally visible Mark
// work to keep the GC main and its requested helpers busy. Avoid turning a
// shallow scheduler tail into a contended allocation-path probe.
inline bool ShouldAdmitMSMarkAssist(uint64_t outstandingTasks,
                                    uint64_t outstandingWorkWords,
                                    size_t targetHelpers)
{
    if (outstandingTasks == 0 || outstandingWorkWords == 0) {
        return false;
    }
    uint64_t gcParticipants = static_cast<uint64_t>(targetHelpers) + 1;
    uint64_t reservedTasks = gcParticipants + 1;
    uint64_t reservedWorkWords = std::max<uint64_t>(
        32, SaturatingMSWUMul(
                gcParticipants, MS_MARK_WORK_WORDS_PER_PARTICIPANT));
    return outstandingTasks > reservedTasks ||
        outstandingWorkWords >= reservedWorkWords;
}

// Per-P Mark debt, prior-work credit, and scheduler shard probe cursor. The
// owner is one AllocBufferMS, so no atomics are needed on the allocation path.
struct MSMarkAssistDebtState {
    uint64_t debtScaled = 0;
    uint64_t creditWU = 0;
    size_t probeCursor = 0;

    void Reset()
    {
        debtScaled = 0;
        creditWU = 0;
        probeCursor = 0;
    }

    void ClearDebtAndCredit()
    {
        debtScaled = 0;
        creditWU = 0;
    }

    void Accumulate(size_t allocatedBytes, uint64_t ratioScaled)
    {
        debtScaled = SaturatingMSWUAdd(
            debtScaled, SaturatingMSWUMul(
                static_cast<uint64_t>(allocatedBytes), ratioScaled));
    }

    void ConsumeCredit()
    {
        if (creditWU == 0) {
            return;
        }
        uint64_t wholeDebtWU = debtScaled / kMSAssistRatioScale;
        uint64_t consumedWU = std::min(wholeDebtWU, creditWU);
        debtScaled -= consumedWU * kMSAssistRatioScale;
        creditWU -= consumedWU;
    }

    size_t WholeDebtWU() const
    {
        uint64_t debtWU = debtScaled / kMSAssistRatioScale;
        return static_cast<size_t>(std::min<uint64_t>(
            debtWU, std::numeric_limits<size_t>::max()));
    }

    void AccountCompletedWork(uint64_t completedWU)
    {
        if (completedWU == 0) {
            return;
        }
        uint64_t wholeDebtWU = debtScaled / kMSAssistRatioScale;
        uint64_t paidWU = std::min(wholeDebtWU, completedWU);
        debtScaled -= paidWU * kMSAssistRatioScale;
        creditWU = SaturatingMSWUAdd(creditWU, completedWU - paidWU);
    }
};

// Sweep-assist pacing uses the same per-P accounting shape as Mark assist,
// but its unit is a Region and completed work can first arrive as shared
// central credit from background/lazy/helper sweeping. Its LOW/MEDIUM/HIGH
// mode is owned by MSSweepPressurePolicy, not by assist debt accounting.
struct MSSweepAssistResult;

constexpr uint64_t kMSSweepAssistRatioScale = uint64_t {1} << 16;

inline uint64_t ComputeMSSweepFractionScaled(uint64_t remainder,
                                              uint64_t denominator)
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

inline uint64_t ComputeMSSweepAssistRatioScaled(
    size_t totalUnsweptRegions, size_t runwayRegions)
{
    if (totalUnsweptRegions == 0) {
        return 0;
    }
    const uint64_t numerator = static_cast<uint64_t>(totalUnsweptRegions);
    const uint64_t denominator = static_cast<uint64_t>(
        std::max<size_t>(runwayRegions, 1));
    const uint64_t whole = numerator / denominator;
    const uint64_t base = SaturatingMultiplyMSSweepValue(
        whole, kMSSweepAssistRatioScale);
    if (base == std::numeric_limits<uint64_t>::max()) {
        return base;
    }
    const uint64_t fraction = ComputeMSSweepFractionScaled(
        numerator % denominator, denominator);
    return SaturatingAddMSSweepValue(base, fraction);
}

inline uint64_t ScaleMSSweepAssistRatioScaled(
    uint64_t ratioScaled, SweepPressureMode mode)
{
    return SaturatingMultiplyMSSweepValue(
        ratioScaled, SweepTierMultiplier(mode));
}

// Per-P Sweep debt is deliberately separate from Mark debt: it resets when a
// new sweep epoch is published, has no overpayment credit, and is paid in
// whole Regions after central credit and direct sweep are combined.
struct MSSweepAssistDebtState {
    uint64_t debtScaled = 0;
    uint32_t generation = 0;

    void ResetForGeneration(uint32_t currentGeneration)
    {
        if (generation != currentGeneration) {
            debtScaled = 0;
            generation = currentGeneration;
        }
    }

    void ClearDebt()
    {
        debtScaled = 0;
    }

    void Accumulate(size_t allocatedRegions, uint64_t ratioScaled)
    {
        debtScaled = SaturatingAddMSSweepValue(
            debtScaled, SaturatingMultiplyMSSweepValue(
                static_cast<uint64_t>(allocatedRegions), ratioScaled));
    }

    size_t WholeDebtRegions() const
    {
        uint64_t debtRegions = debtScaled / kMSSweepAssistRatioScale;
        return static_cast<size_t>(std::min<uint64_t>(
            debtRegions, std::numeric_limits<size_t>::max()));
    }

    void AccountPaidRegions(size_t paidRegions)
    {
        uint64_t paidScaled = SaturatingMultiplyMSSweepValue(
            static_cast<uint64_t>(paidRegions), kMSSweepAssistRatioScale);
        debtScaled = paidScaled >= debtScaled ? 0 : debtScaled - paidScaled;
    }

    void AccountSweepResult(const MSSweepAssistResult& result);
};

inline size_t ComputeMSSweepAssistDirectRegions(size_t unpaidDebtRegions,
                                                 size_t configuredBatchRegions)
{
    size_t batchRegions = std::max<size_t>(configuredBatchRegions, 1);
    return unpaidDebtRegions < batchRegions ? 0 : batchRegions;
}

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
            if (credits_.compare_exchange_weak(
                    current, desired, std::memory_order_release,
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
            if (credits_.compare_exchange_weak(
                    current, current - claimed, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return claimed;
            }
        }
        return 0;
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
        return assistPaidRegions > max - creditPaidRegions
            ? max
            : creditPaidRegions + assistPaidRegions;
    }
};

inline void MSSweepAssistDebtState::AccountSweepResult(
    const MSSweepAssistResult& result)
{
    if (result.sweepComplete) {
        ClearDebt();
        return;
    }
    AccountPaidRegions(result.TotalPaidRegions());
}

class AllocBufferMS;
class CentralRegionPool;

// These slow-path entry points retain the real scan operations in the owning
// Collector/Pool while centralizing each P's debt/credit state transition.
void UpdateMSMarkAssistDebt(MSMarkAssistDebtState& state,
                            AllocBufferMS& allocBuffer,
                            size_t allocatedBytes);
void UpdateMSSweepAssistDebt(MSSweepAssistDebtState& state,
                             CentralRegionPool* centralPool,
                             size_t allocatedRegions,
                             uint8_t preferredCls);

} // namespace MapleRuntime

#endif // MRT_MS_ASSIST_PACING_H
