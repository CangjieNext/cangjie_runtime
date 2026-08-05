// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_WORK_UNIT_H
#define MRT_MS_WORK_UNIT_H

#ifdef MRT_USE_MARK_SWEEP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace MapleRuntime {

constexpr uint64_t MS_WU_Q16_ONE = UINT64_C(1) << 16;
constexpr uint64_t MS_WU_RESOLVED_REF_COST = 1;
constexpr uint64_t MS_WU_MARK_WORD_RMW_COST = 8;
constexpr uint64_t MS_WU_SCAN_WORD_COST = 1;
constexpr uint64_t MS_WU_INITIAL_DENSITY_Q16 = MS_WU_Q16_ONE / 64;
constexpr uint64_t MS_WU_INITIAL_THROUGHPUT_Q16 = 32 * MS_WU_Q16_ONE;
constexpr uint64_t MS_WU_ASSIST_THRESHOLD = 2048;
constexpr uint64_t MS_WU_MAIN_WALL_BUDGET_US = 100;
constexpr uint64_t MS_WU_HELPER_WALL_BUDGET_US = 100;
constexpr uint64_t MS_WU_ASSIST_WALL_BUDGET_US = 100;
constexpr uint64_t MS_WU_MIN_SLICE_BUDGET = 64;
constexpr uint64_t MS_WU_UNLIMITED_BUDGET =
    std::numeric_limits<uint64_t>::max();
constexpr uint64_t MS_MARK_WORK_WORDS_PER_PARTICIPANT = 128;
constexpr size_t MS_MARK_CONTROL_SLICES_PER_UPDATE = 4;
constexpr uint8_t MS_MARK_HELPER_UP_WINDOWS = 4;
constexpr uint8_t MS_MARK_HELPER_DOWN_WINDOWS = 3;
constexpr size_t MS_MARK_NORMAL_INITIAL_HELPERS = 2;
constexpr uint8_t MS_MARK_ASSIST_UP_WINDOWS = 2;
constexpr uint8_t MS_MARK_ASSIST_DOWN_WINDOWS = 2;
constexpr uint64_t MS_MARK_MIN_TREND_DEADBAND_WORDS = 32;

inline uint64_t SaturatingMSWUAdd(uint64_t lhs, uint64_t rhs)
{
    return rhs > std::numeric_limits<uint64_t>::max() - lhs
        ? std::numeric_limits<uint64_t>::max()
        : lhs + rhs;
}

inline uint64_t SaturatingMSWUMul(uint64_t lhs, uint64_t rhs)
{
    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    return lhs > std::numeric_limits<uint64_t>::max() / rhs
        ? std::numeric_limits<uint64_t>::max()
        : lhs * rhs;
}

inline uint64_t ComputeMSMarkAssistSliceBudget(uint64_t debtWU)
{
    return std::max<uint64_t>(MS_WU_ASSIST_THRESHOLD, debtWU);
}

inline void ConsumeMSMarkAssistCredit(
    uint64_t& debtScaled, uint64_t& creditWU, uint64_t ratioScale)
{
    if (ratioScale == 0 || creditWU == 0) {
        return;
    }
    uint64_t wholeDebtWU = debtScaled / ratioScale;
    uint64_t consumedWU = std::min(wholeDebtWU, creditWU);
    debtScaled -= consumedWU * ratioScale;
    creditWU -= consumedWU;
}

inline void AccountMSMarkAssistWork(
    uint64_t& debtScaled, uint64_t& creditWU,
    uint64_t completedWU, uint64_t ratioScale)
{
    if (completedWU == 0) {
        return;
    }
    uint64_t wholeDebtWU = ratioScale == 0 ? 0 : debtScaled / ratioScale;
    uint64_t paidWU = std::min(wholeDebtWU, completedWU);
    debtScaled -= paidWU * ratioScale;
    creditWU = SaturatingMSWUAdd(creditWU, completedWU - paidWU);
}

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

inline uint64_t MSWUQ16MulInteger(uint64_t q16, uint64_t value)
{
    __uint128_t product = static_cast<__uint128_t>(q16) * value;
    product >>= 16;
    return product > std::numeric_limits<uint64_t>::max()
        ? std::numeric_limits<uint64_t>::max()
        : static_cast<uint64_t>(product);
}

inline uint64_t UpdateMSWUEwmaOneEighth(uint64_t previous, uint64_t sample)
{
    if (sample >= previous) {
        return SaturatingMSWUAdd(previous, (sample - previous) / 8);
    }
    return previous - (previous - sample) / 8;
}

struct MSWorkUnitCounter {
    uint64_t totalWU = 0;

    void AddResolvedRefs(uint64_t count = 1)
    {
        totalWU += count * MS_WU_RESOLVED_REF_COST;
    }

    void AddMarkWordRMW(uint64_t count = 1)
    {
        totalWU += count * MS_WU_MARK_WORD_RMW_COST;
    }

    void AddScanWords(uint64_t count = 1)
    {
        totalWU += count * MS_WU_SCAN_WORD_COST;
    }

    uint64_t Total() const
    {
        // This is a task-local counter. A task cannot process enough heap
        // objects to approach uint64_t overflow on the supported 64-bit heap,
        // so keep the per-reference path branch-free. Cross-task aggregation
        // still uses the saturating helpers above.
        return totalWU;
    }

    void Reset()
    {
        totalWU = 0;
    }
};

constexpr uint64_t MS_MARK_HIGH_RUNWAY_US = 6 * 1000 * 1000;
constexpr uint64_t MS_MARK_CRITICAL_RUNWAY_US = 3 * 1000 * 1000;
constexpr uint8_t MS_MARK_HIGH_PRESSURE_WINDOWS = 3;
constexpr uint8_t MS_MARK_CRITICAL_PRESSURE_WINDOWS = 2;

enum class MSMarkPressure : uint8_t {
    NORMAL,
    HIGH,
    CRITICAL,
};

inline MSMarkPressure ClassifyMSMarkPressure(uint64_t runwayUs, bool hasAllocation)
{
    if (!hasAllocation || runwayUs > MS_MARK_HIGH_RUNWAY_US) {
        return MSMarkPressure::NORMAL;
    }
    if (runwayUs > MS_MARK_CRITICAL_RUNWAY_US) {
        return MSMarkPressure::HIGH;
    }
    return MSMarkPressure::CRITICAL;
}

inline uint64_t ComputeMSMarkRunwayUs(uint64_t heapRunwayBytes,
                                      uint64_t allocatedBytesPerUs)
{
    return allocatedBytesPerUs == 0
        ? std::numeric_limits<uint64_t>::max()
        : heapRunwayBytes / allocatedBytesPerUs;
}

// Runway estimates are sampled while concurrent marking is active and can
// spike when a mutator briefly allocates faster than the rolling average.
// Require sustained observations before widening the worker set so a single
// sample cannot jump directly to half (or all) of the available helpers.
class MSMarkPressureGate {
public:
    explicit MSMarkPressureGate(MSMarkPressure initialPressure)
        : acceptedPressure_(initialPressure)
    {
    }

    MSMarkPressure Observe(MSMarkPressure observedPressure)
    {
        if (acceptedPressure_ == MSMarkPressure::CRITICAL) {
            return acceptedPressure_;
        }
        if (observedPressure == MSMarkPressure::NORMAL) {
            highWindows_ = 0;
            criticalWindows_ = 0;
            return acceptedPressure_;
        }

        ++highWindows_;
        if (observedPressure == MSMarkPressure::CRITICAL) {
            ++criticalWindows_;
            if (criticalWindows_ >= MS_MARK_CRITICAL_PRESSURE_WINDOWS) {
                acceptedPressure_ = MSMarkPressure::CRITICAL;
                return acceptedPressure_;
            }
        } else {
            criticalWindows_ = 0;
        }

        if (acceptedPressure_ == MSMarkPressure::NORMAL &&
            highWindows_ >= MS_MARK_HIGH_PRESSURE_WINDOWS) {
            acceptedPressure_ = MSMarkPressure::HIGH;
        }
        return acceptedPressure_;
    }

private:
    MSMarkPressure acceptedPressure_ = MSMarkPressure::NORMAL;
    uint8_t highWindows_ = 0;
    uint8_t criticalWindows_ = 0;
};

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

class MSMarkControllerState {
public:
    MSMarkControllerState(
        uint64_t outstandingTasks, uint64_t outstandingWorkWords,
        size_t maxHelpers, size_t maxAssistMultiplier,
        MSMarkPressure pressure)
        : maxHelpers_(maxHelpers),
          maxAssistMultiplier_(std::max<size_t>(1, maxAssistMultiplier)),
          pressure_(pressure),
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

    void Observe(uint64_t outstandingTasks, uint64_t outstandingWorkWords)
    {
        uint64_t previousFilteredWorkWords = filteredWorkWords_;
        filteredWorkWords_ = UpdateMSMarkWorkWordsEwma(
            filteredWorkWords_, outstandingWorkWords);

        size_t desiredHelpers = ComputeMSMarkHelperTarget(
            outstandingTasks, filteredWorkWords_,
            maxHelpers_, pressure_);
        UpdateHelpers(desiredHelpers);
        UpdateAssist(previousFilteredWorkWords);
    }

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

    size_t HelperTarget() const
    {
        return helperTarget_;
    }

    size_t AssistMultiplier() const
    {
        return assistMultiplier_;
    }

    uint64_t FilteredWorkWords() const
    {
        return filteredWorkWords_;
    }

private:
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
    uint64_t filteredWorkWords_ = 0;
    size_t helperTarget_ = 0;
    size_t minAssistMultiplier_ = 1;
    size_t assistMultiplier_ = 1;
    uint8_t helperUpWindows_ = 0;
    uint8_t helperDownWindows_ = 0;
    uint8_t helperStableWindows_ = 0;
    uint8_t assistUpWindows_ = 0;
    uint8_t assistDownWindows_ = 0;
};

inline uint64_t ComputeMSMarkShareBasisPoints(
    uint64_t participantWU, uint64_t totalWU)
{
    if (totalWU == 0) {
        return 0;
    }
    __uint128_t scaled =
        static_cast<__uint128_t>(participantWU) * 10000;
    return static_cast<uint64_t>(scaled / totalWU);
}

inline uint64_t ComputeMSMarkRateQ16(uint64_t completedWU,
                                     uint64_t activeUs)
{
    return activeUs == 0 ? 0 : MSWUQ16Divide(completedWU, activeUs);
}

inline uint64_t ComputeMSMarkSliceBudget(
    uint64_t throughputQ16, uint64_t wallBudgetUs)
{
    return std::max(
        MS_WU_MIN_SLICE_BUDGET,
        MSWUQ16MulInteger(throughputQ16, wallBudgetUs));
}

inline uint64_t ComputeMSAssistRatioQ16(
    uint64_t workDensityQ16, MSMarkPressure pressure, uint64_t outstanding)
{
    if (outstanding == 0) {
        return 0;
    }
    if (pressure == MSMarkPressure::CRITICAL) {
        return workDensityQ16;
    }
    return pressure == MSMarkPressure::HIGH
        ? workDensityQ16 / 2
        : workDensityQ16 / 4;
}

class MSWorkUnitPacer {
public:
    uint64_t GetWorkDensityQ16() const { return workDensityQ16_; }
    uint64_t GetThroughputQ16() const { return throughputQ16_; }

    void UpdateFullCycle(
        uint64_t completedWU, uint64_t allocatedBytes, uint64_t backgroundUs)
    {
        if (allocatedBytes != 0) {
            workDensityQ16_ = UpdateMSWUEwmaOneEighth(
                workDensityQ16_, MSWUQ16Divide(completedWU, allocatedBytes));
        }
        if (backgroundUs != 0) {
            throughputQ16_ = UpdateMSWUEwmaOneEighth(
                throughputQ16_, MSWUQ16Divide(completedWU, backgroundUs));
        }
    }

private:
    uint64_t workDensityQ16_ = MS_WU_INITIAL_DENSITY_Q16;
    uint64_t throughputQ16_ = MS_WU_INITIAL_THROUGHPUT_Q16;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_WORK_UNIT_H
