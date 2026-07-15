// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_PARALLEL_SWEEP_POLICY_H
#define MRT_MS_PARALLEL_SWEEP_POLICY_H

#ifdef MRT_USE_MARK_SWEEP

#include <algorithm>
#include <cstddef>
#include <limits>

namespace MapleRuntime {

constexpr size_t MS_PARALLEL_SWEEP_REGIONS_PER_HELPER = 64;
constexpr size_t MS_PARALLEL_SWEEP_TARGET_WAVES = 4;
constexpr size_t MS_PARALLEL_SWEEP_EMERGENCY_CAP_MULTIPLIER = 10;

struct MSParallelSweepPlan {
    MSParallelSweepPlan(size_t helpers = 0, size_t workers = 1, size_t quantum = 1)
        : borrowedHelpers(helpers), totalWorkers(workers), regionQuantum(quantum)
    {}

    size_t borrowedHelpers;
    size_t totalWorkers;
    size_t regionQuantum;
};

inline size_t SaturatingMultiply(size_t lhs, size_t rhs)
{
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        return std::numeric_limits<size_t>::max();
    }
    return lhs * rhs;
}

inline MSParallelSweepPlan ComputeMSParallelSweepPlan(
    size_t unsweptRegions, size_t gcThreads, size_t configuredMaxHelpers,
    size_t assistMaxRegions)
{
    size_t logicalMaxHelpers = gcThreads > 1 ? gcThreads - 1 : 0;
    if (configuredMaxHelpers != 0) {
        logicalMaxHelpers = std::min(logicalMaxHelpers, configuredMaxHelpers);
    }

    // Keep one coordinator-only lane through 64 regions. Every additional
    // 64-region band borrows one physical GC helper: 65->1, 129->2, 193->3.
    size_t requestedHelpers = unsweptRegions == 0 ? 0 :
        (unsweptRegions - 1) / MS_PARALLEL_SWEEP_REGIONS_PER_HELPER;
    size_t borrowedHelpers = std::min(logicalMaxHelpers, requestedHelpers);
    size_t totalWorkers = 1 + borrowedHelpers;

    size_t waves = SaturatingMultiply(totalWorkers, MS_PARALLEL_SWEEP_TARGET_WAVES);
    size_t targetPerTask = unsweptRegions == 0 ? 1 : 1 + (unsweptRegions - 1) / waves;
    size_t emergencyCap = SaturatingMultiply(
        assistMaxRegions, MS_PARALLEL_SWEEP_EMERGENCY_CAP_MULTIPLIER);
    emergencyCap = std::max<size_t>(1, emergencyCap);
    size_t regionQuantum = std::max<size_t>(1, std::min(targetPerTask, emergencyCap));

    return MSParallelSweepPlan(borrowedHelpers, totalWorkers, regionQuantum);
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
