// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_PARALLEL_SWEEP_POLICY_H
#define MRT_MS_PARALLEL_SWEEP_POLICY_H

#ifdef MRT_USE_MARK_SWEEP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "Heap/MAllocator/MSSweepDebtPolicy.h"

namespace MapleRuntime {

inline size_t SaturatingMultiply(size_t lhs, size_t rhs)
{
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        return std::numeric_limits<size_t>::max();
    }
    return lhs * rhs;
}

inline size_t ComputeMSMaxSweepHelpers(size_t gcThreads, size_t configuredMaxHelpers)
{
    size_t logicalMaxHelpers = gcThreads > 1 ? gcThreads - 1 : 0;
    if (configuredMaxHelpers != 0) {
        logicalMaxHelpers = std::min(logicalMaxHelpers, configuredMaxHelpers);
    }
    return logicalMaxHelpers;
}

inline size_t ComputeMSSweepExcessBytes(size_t sweptBytes, size_t unsweptBytes,
                                        size_t thresholdBytes)
{
    if (sweptBytes >= thresholdBytes) {
        return SaturatingAddMSSweepValue(sweptBytes - thresholdBytes, unsweptBytes);
    }
    size_t remaining = thresholdBytes - sweptBytes;
    return unsweptBytes > remaining ? unsweptBytes - remaining : 0;
}

// Return ceil(excess / threshold * maxHelpers), clamped to [1, maxHelpers].
// maxHelpers originates in the int32_t GC thread count, so the remainder
// product below is bounded by INT32_MAX^2 and cannot overflow size_t.
inline size_t ComputeMSMediumSweepTargetHelpers(size_t sweptBytes, size_t unsweptBytes,
                                                size_t thresholdBytes, size_t maxHelpers)
{
    maxHelpers = std::min(maxHelpers, static_cast<size_t>(std::numeric_limits<int32_t>::max()));
    if (maxHelpers == 0) {
        return 0;
    }
    size_t excessBytes = ComputeMSSweepExcessBytes(sweptBytes, unsweptBytes, thresholdBytes);
    if (thresholdBytes == 0 || excessBytes >= thresholdBytes) {
        return maxHelpers;
    }
    if (excessBytes == 0) {
        return 1;
    }

    size_t thresholdQuotient = thresholdBytes / maxHelpers;
    size_t thresholdRemainder = thresholdBytes % maxHelpers;
    size_t low = 1;
    size_t high = maxHelpers;
    while (low < high) {
        size_t candidate = low + (high - low) / 2;
        size_t boundary = candidate * thresholdQuotient +
            (candidate * thresholdRemainder) / maxHelpers;
        if (excessBytes <= boundary) {
            high = candidate;
        } else {
            low = candidate + 1;
        }
    }
    return low;
}

inline size_t ComputeMSSweepTargetHelpers(SweepPressureMode mode, size_t sweptBytes,
                                          size_t unsweptBytes, size_t thresholdBytes,
                                          size_t maxHelpers)
{
    switch (mode) {
        case SweepPressureMode::HIGH:
            return maxHelpers;
        case SweepPressureMode::MEDIUM:
            return ComputeMSMediumSweepTargetHelpers(
                sweptBytes, unsweptBytes, thresholdBytes, maxHelpers);
        case SweepPressureMode::LOW:
        default:
            return 0;
    }
}

inline size_t ComputeMSActiveSweepHelpers(size_t currentHelpers, size_t targetHelpers,
                                          SweepPressureMode mode, bool forcedHigh)
{
    if (forcedHigh || mode == SweepPressureMode::HIGH) {
        return targetHelpers;
    }
    if (currentHelpers < targetHelpers) {
        return currentHelpers + 1;
    }
    if (currentHelpers > targetHelpers) {
        return currentHelpers - 1;
    }
    return currentHelpers;
}

inline size_t ComputeMSSweepSliceBudgetUs(size_t baseBudgetUs, SweepPressureMode mode)
{
    return SaturatingMultiply(baseBudgetUs, SweepTierMultiplier(mode));
}

inline bool IsMSSweepQuiescent(bool sweepDone, bool backgroundRunning,
                               size_t activeLazy, size_t activeAssist, size_t activeHelper)
{
    return sweepDone && !backgroundRunning && activeLazy == 0 &&
           activeAssist == 0 && activeHelper == 0;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_PARALLEL_SWEEP_POLICY_H
