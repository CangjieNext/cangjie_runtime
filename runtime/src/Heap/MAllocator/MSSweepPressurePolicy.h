// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_SWEEP_PRESSURE_POLICY_H
#define MRT_MS_SWEEP_PRESSURE_POLICY_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>
#include <limits>

namespace MapleRuntime {

// Sweep pressure belongs to the central Region-pool/backlog policy. Assist
// debt consumes this mode, but does not define it.
enum class SweepPressureMode : int {
    LOW = 0,
    MEDIUM = 1,
    HIGH = 2,
};

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

// Sweep scheduling uses native-size byte and time budgets. Keep its overflow
// rule next to the other sweep arithmetic rather than duplicating it in each
// policy consumer.
inline size_t SaturatingMultiplyMSSweepBytes(size_t lhs, size_t rhs)
{
    constexpr size_t max = std::numeric_limits<size_t>::max();
    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    return lhs > max / rhs ? max : lhs * rhs;
}

inline size_t ComputeMSSweepRunwayBytes(size_t thresholdBytes, size_t sweptBytes)
{
    return thresholdBytes > sweptBytes ? thresholdBytes - sweptBytes : 0;
}

inline size_t CeilDivideMSSweepValue(size_t value, size_t divisor)
{
    return value / divisor + static_cast<size_t>(value % divisor != 0);
}

inline SweepPressureMode EvaluateSweepPressure(
    size_t unsweptBytes, size_t sweptBytes, size_t thresholdBytes,
    bool forcedHigh, SweepPressureMode previousMode)
{
    if (unsweptBytes == 0) {
        return SweepPressureMode::LOW;
    }
    if (forcedHigh) {
        return SweepPressureMode::HIGH;
    }

    size_t runwayBytes = ComputeMSSweepRunwayBytes(
        thresholdBytes, sweptBytes);
    if (runwayBytes == 0 || unsweptBytes >= runwayBytes) {
        return SweepPressureMode::HIGH;
    }

    size_t highHoldBytes = CeilDivideMSSweepValue(runwayBytes, 2);
    if (previousMode == SweepPressureMode::HIGH &&
        unsweptBytes >= highHoldBytes) {
        return SweepPressureMode::HIGH;
    }
    if (unsweptBytes >= highHoldBytes) {
        return SweepPressureMode::MEDIUM;
    }

    size_t mediumHoldBytes = CeilDivideMSSweepValue(runwayBytes, 4);
    if (previousMode != SweepPressureMode::LOW &&
        unsweptBytes >= mediumHoldBytes) {
        return SweepPressureMode::MEDIUM;
    }
    return SweepPressureMode::LOW;
}

inline size_t SweepTierMultiplier(SweepPressureMode mode)
{
    switch (mode) {
        case SweepPressureMode::MEDIUM:
            return 4;
        case SweepPressureMode::HIGH:
            return 10;
        case SweepPressureMode::LOW:
        default:
            return 1;
    }
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_SWEEP_PRESSURE_POLICY_H
