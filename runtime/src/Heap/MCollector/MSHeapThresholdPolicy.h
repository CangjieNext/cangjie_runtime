// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_HEAP_THRESHOLD_POLICY_H
#define MRT_MS_HEAP_THRESHOLD_POLICY_H

#include <algorithm>
#include <cstddef>
#include <limits>

namespace MapleRuntime {

constexpr long double kMSHeapThresholdMaxRisePerCycle = 1.125L;
constexpr long double kMSHeapThresholdLiveSafetyFactor = 1.25L;

struct MSHeapThresholdDecision {
    size_t rawTarget = 0;
    size_t riseLimit = 0;
    size_t liveSafetyFloor = 0;
    size_t threshold = 0;
    bool riseLimited = false;
};

inline size_t ScaleMSHeapThreshold(size_t value, long double multiplier)
{
    if (multiplier <= 0.0L || value == 0) {
        return 0;
    }
    long double scaled = static_cast<long double>(value) * multiplier;
    if (scaled >= static_cast<long double>(std::numeric_limits<size_t>::max())) {
        return std::numeric_limits<size_t>::max();
    }
    return static_cast<size_t>(scaled);
}

inline MSHeapThresholdDecision EvaluateMSHeapThreshold(size_t oldThreshold, size_t liveBytes,
                                                       double heapGrowth, size_t minThreshold,
                                                       size_t maxThreshold)
{
    MSHeapThresholdDecision decision;
    minThreshold = std::min(minThreshold, maxThreshold);
    decision.rawTarget = ScaleMSHeapThreshold(liveBytes, 1.0L + static_cast<long double>(heapGrowth));
    decision.rawTarget = std::max(minThreshold, std::min(decision.rawTarget, maxThreshold));

    decision.riseLimit =
        std::min(ScaleMSHeapThreshold(oldThreshold, kMSHeapThresholdMaxRisePerCycle), maxThreshold);
    decision.liveSafetyFloor =
        std::min(ScaleMSHeapThreshold(liveBytes, kMSHeapThresholdLiveSafetyFactor), maxThreshold);
    if (decision.rawTarget <= oldThreshold) {
        decision.threshold = decision.rawTarget;
        return decision;
    }

    size_t allowedThreshold = std::max(decision.riseLimit, decision.liveSafetyFloor);
    decision.threshold = std::min(decision.rawTarget, allowedThreshold);
    decision.riseLimited = decision.threshold < decision.rawTarget;
    return decision;
}

inline size_t ComputeMSHeapThreshold(size_t oldThreshold, size_t liveBytes, double heapGrowth,
                                     size_t minThreshold, size_t maxThreshold)
{
    return EvaluateMSHeapThreshold(oldThreshold, liveBytes, heapGrowth, minThreshold, maxThreshold).threshold;
}

} // namespace MapleRuntime

#endif // MRT_MS_HEAP_THRESHOLD_POLICY_H
