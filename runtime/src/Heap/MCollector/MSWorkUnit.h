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
constexpr uint64_t MS_MARK_OUTSTANDING_TASKS_PER_LEVEL = 8;

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

enum class MSMarkPressure : uint8_t {
    NORMAL,
    HIGH,
    CRITICAL,
};

inline MSMarkPressure ClassifyMSMarkPressure(uint64_t runwayUs, bool hasAllocation)
{
    if (!hasAllocation || runwayUs > 100000) {
        return MSMarkPressure::NORMAL;
    }
    if (runwayUs >= 20000) {
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

inline size_t ComputeMSMarkOutstandingLevel(uint64_t outstanding)
{
    if (outstanding == 0) {
        return 0;
    }
    uint64_t level = 1 +
        (outstanding - 1) / MS_MARK_OUTSTANDING_TASKS_PER_LEVEL;
    return static_cast<size_t>(std::min<uint64_t>(
        level, std::numeric_limits<size_t>::max()));
}

inline size_t ComputeMSMarkHelperTarget(
    uint64_t outstanding, size_t maxHelpers, MSMarkPressure pressure)
{
    if (outstanding == 0 || maxHelpers == 0) {
        return 0;
    }
    // Outstanding already includes queued and leased tasks. Scale from that
    // single ledger value so ready-stack hints cannot double count backlog.
    size_t target = std::min(
        ComputeMSMarkOutstandingLevel(outstanding), maxHelpers);
    target = std::max<size_t>(1, target);
    if (pressure == MSMarkPressure::HIGH) {
        target = std::max(target, (maxHelpers + 1) / 2);
    } else if (pressure == MSMarkPressure::CRITICAL) {
        target = maxHelpers;
    }
    size_t outstandingLimit = static_cast<size_t>(
        std::min<uint64_t>(outstanding, std::numeric_limits<size_t>::max()));
    return std::min(target, std::min(maxHelpers, outstandingLimit));
}

inline size_t ComputeMSMarkAssistMultiplier(
    uint64_t outstanding, size_t maxMultiplier, MSMarkPressure pressure)
{
    maxMultiplier = std::max<size_t>(1, maxMultiplier);
    size_t multiplier = std::max<size_t>(
        1, std::min(ComputeMSMarkOutstandingLevel(outstanding), maxMultiplier));
    if (pressure == MSMarkPressure::HIGH) {
        multiplier = std::max(multiplier, (maxMultiplier + 1) / 2);
    } else if (pressure == MSMarkPressure::CRITICAL) {
        multiplier = maxMultiplier;
    }
    return std::min(multiplier, maxMultiplier);
}

inline uint64_t ComputeMSMarkDynamicAssistRatioQ16(
    uint64_t baseRatio, uint64_t outstanding, size_t maxMultiplier,
    MSMarkPressure pressure)
{
    return SaturatingMSWUMul(
        baseRatio,
        ComputeMSMarkAssistMultiplier(
            outstanding, maxMultiplier, pressure));
}

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
