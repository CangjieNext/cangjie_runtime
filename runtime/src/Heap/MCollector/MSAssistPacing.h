// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_ASSIST_PACING_H
#define MRT_MS_ASSIST_PACING_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace MapleRuntime {

constexpr uint64_t kMSAssistRatioScale = 65536;
constexpr uint64_t kMSAssistBaseRatioPercentDefault = 50;

inline uint64_t ComputeMSAssistBaseRatioScaled(size_t prevMarked, size_t allocatedBytes, uint64_t basePercent)
{
    if (prevMarked == 0 || allocatedBytes == 0 || basePercent == 0) {
        return 1;
    }

    long double scaled = static_cast<long double>(prevMarked) * static_cast<long double>(kMSAssistRatioScale) *
                         static_cast<long double>(basePercent) /
                         (static_cast<long double>(allocatedBytes) * 100.0L);
    if (scaled >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return std::numeric_limits<uint64_t>::max();
    }
    return std::max<uint64_t>(1, static_cast<uint64_t>(scaled));
}

inline double UpdateMSAssistBacklogEMA(double previous, size_t current, bool initialized)
{
    if (!initialized) {
        return static_cast<double>(current);
    }
    return previous * 0.875 + static_cast<double>(current) * 0.125;
}

inline uint64_t ComputeMSAssistBacklogRatioScaled(uint64_t baseRatio, double smoothedBacklog,
                                                  size_t backlogStepBuffers, size_t maxMultiplier)
{
    if (baseRatio == 0 || backlogStepBuffers == 0 || maxMultiplier == 0) {
        return baseRatio;
    }

    long double multiplier = 1.0L + static_cast<long double>(smoothedBacklog) /
                             static_cast<long double>(backlogStepBuffers);
    multiplier = std::min(multiplier, static_cast<long double>(maxMultiplier));
    long double adjusted = static_cast<long double>(baseRatio) * multiplier;
    if (adjusted >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(adjusted);
}

} // namespace MapleRuntime

#endif // MRT_MS_ASSIST_PACING_H
