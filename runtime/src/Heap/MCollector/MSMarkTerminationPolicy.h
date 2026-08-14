// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_TERMINATION_POLICY_H
#define MRT_MS_MARK_TERMINATION_POLICY_H

#include <algorithm>
#include <cstddef>

namespace MapleRuntime {

// MarkTermination uses this only to select the STW helper width for its final
// drain. Concurrent Mark pressure and assist multipliers are owned by
// MSAssistPacing.h and deliberately do not leak into this policy.
constexpr size_t ComputeMSMarkTerminationDrainLevel(
    size_t residual, size_t firstHelperThreshold)
{
    if (residual < firstHelperThreshold) {
        return 0;
    }
    size_t level = 1;
    size_t threshold = firstHelperThreshold;
    while (threshold <= residual / 2) {
        threshold *= 2;
        ++level;
    }
    return level;
}

constexpr size_t ComputeMSMarkTerminationDrainHelpers(
    size_t residualTasks, size_t residualWorkWords, size_t maxHelpers)
{
    constexpr size_t kFirstHelperTaskThreshold = 64;
    constexpr size_t kFirstHelperWorkWordThreshold = 256;
    size_t taskLevel = ComputeMSMarkTerminationDrainLevel(
        residualTasks, kFirstHelperTaskThreshold);
    size_t workLevel = ComputeMSMarkTerminationDrainLevel(
        residualWorkWords, kFirstHelperWorkWordThreshold);
    return std::min(maxHelpers, std::max(taskLevel, workLevel));
}

} // namespace MapleRuntime

#endif // MRT_MS_MARK_TERMINATION_POLICY_H
