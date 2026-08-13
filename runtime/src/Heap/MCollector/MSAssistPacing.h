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
constexpr uint64_t kMSAssistTargetShareBasisPoints = 8000;
constexpr uint64_t kMSAssistMaxShareBasisPoints = 9000;

struct MSAssistBaseRatioDecision {
    uint64_t rawRatio = 0;
    uint64_t previousRatio = 0;
    uint64_t riseLimit = 0;
    uint64_t feedbackLimit = 0;
    uint64_t nextRatio = 0;
    uint64_t assistShareBasisPoints = 0;
    bool riseLimited = false;
    bool shareLimited = false;
};

inline uint64_t ApplyMSAssistDisableOverride(uint64_t ratio, bool disableAssist)
{
    return disableAssist ? 0 : ratio;
}

inline MSAssistBaseRatioDecision EvaluateMSAssistBaseRatio(uint64_t rawRatio, uint64_t previousRatio,
                                                           bool initialized, size_t assistProcessed,
                                                           size_t backgroundProcessed)
{
    MSAssistBaseRatioDecision decision;
    decision.rawRatio = std::max<uint64_t>(1, rawRatio);
    decision.previousRatio = std::max<uint64_t>(1, previousRatio);
    decision.riseLimit = decision.rawRatio;
    decision.feedbackLimit = decision.rawRatio;
    decision.nextRatio = decision.rawRatio;

    if (!initialized) {
        return decision;
    }

    if (decision.rawRatio > decision.previousRatio) {
        uint64_t rise = std::max<uint64_t>(1, decision.previousRatio / 4);
        decision.riseLimit = decision.previousRatio > std::numeric_limits<uint64_t>::max() - rise
            ? std::numeric_limits<uint64_t>::max()
            : decision.previousRatio + rise;
        if (decision.nextRatio > decision.riseLimit) {
            decision.nextRatio = decision.riseLimit;
            decision.riseLimited = true;
        }
    }

    long double totalProcessed = static_cast<long double>(assistProcessed) +
                                 static_cast<long double>(backgroundProcessed);
    if (totalProcessed > 0.0L) {
        long double share = static_cast<long double>(assistProcessed) * 10000.0L / totalProcessed;
        decision.assistShareBasisPoints = static_cast<uint64_t>(std::min<long double>(share, 10000.0L));
    }
    if (decision.assistShareBasisPoints > kMSAssistMaxShareBasisPoints) {
        long double feedback = static_cast<long double>(decision.previousRatio) *
                               static_cast<long double>(kMSAssistTargetShareBasisPoints) /
                               static_cast<long double>(decision.assistShareBasisPoints);
        decision.feedbackLimit = std::max<uint64_t>(1, static_cast<uint64_t>(feedback));
        if (decision.nextRatio > decision.feedbackLimit) {
            decision.nextRatio = decision.feedbackLimit;
            decision.shareLimited = true;
        }
    }
    return decision;
}

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
