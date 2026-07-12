// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_WORKER_POLICY_H
#define MRT_MS_MARK_WORKER_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

inline size_t ResolveMSMarkTotalThreads(size_t processorCount, size_t configuredThreads)
{
    size_t processors = std::max<size_t>(1, processorCount);
    if (configuredThreads != 0) {
        return std::max<size_t>(1, std::min(configuredThreads, processors));
    }
    return std::min<size_t>(4, std::max<size_t>(1, processors / 2));
}

struct MSMarkWorkerDecision {
    enum class Reason : uint8_t {
        HOLD,
        PRESSURE_UP,
        TREND_UP,
        EMERGENCY_UP,
        TAIL_DRAIN,
    };

    size_t activeHelpers = 0;
    Reason reason = Reason::HOLD;
};

class MSMarkWorkerPolicy {
public:
    explicit MSMarkWorkerPolicy(size_t maxHelpers) : maxHelpers_(maxHelpers) {}

    MSMarkWorkerDecision Update(size_t fullBacklog)
    {
        MSMarkWorkerDecision decision { activeHelpers_, MSMarkWorkerDecision::Reason::HOLD };
        if (tailDraining_) {
            previousBacklog_ = fullBacklog;
            return decision;
        }
        if (maxHelpers_ == 0) {
            previousBacklog_ = fullBacklog;
            return decision;
        }

        if (fullBacklog >= kEmergencyBacklog) {
            activeHelpers_ = maxHelpers_;
            growthSamples_ = 0;
            decision = { activeHelpers_, MSMarkWorkerDecision::Reason::EMERGENCY_UP };
        } else {
            if (fullBacklog > previousBacklog_) {
                ++growthSamples_;
            } else {
                growthSamples_ = 0;
            }

            size_t activeTotalThreads = activeHelpers_ + 1;
            if (activeHelpers_ < maxHelpers_ && fullBacklog > kBacklogPerThread * activeTotalThreads) {
                ++activeHelpers_;
                decision = { activeHelpers_, MSMarkWorkerDecision::Reason::PRESSURE_UP };
            } else if (activeHelpers_ < maxHelpers_ && growthSamples_ >= kGrowthSamplesForScaleUp) {
                ++activeHelpers_;
                growthSamples_ = 0;
                decision = { activeHelpers_, MSMarkWorkerDecision::Reason::TREND_UP };
            }
        }

        previousBacklog_ = fullBacklog;
        decision.activeHelpers = activeHelpers_;
        return decision;
    }

    MSMarkWorkerDecision EnterTail()
    {
        tailDraining_ = true;
        activeHelpers_ = maxHelpers_;
        growthSamples_ = 0;
        return { activeHelpers_, MSMarkWorkerDecision::Reason::TAIL_DRAIN };
    }

    size_t GetActiveHelpers() const { return activeHelpers_; }

private:
    static constexpr size_t kBacklogPerThread = 8;
    static constexpr size_t kEmergencyBacklog = 64;
    static constexpr size_t kGrowthSamplesForScaleUp = 2;

    size_t maxHelpers_ = 0;
    size_t activeHelpers_ = 0;
    size_t previousBacklog_ = 0;
    size_t growthSamples_ = 0;
    bool tailDraining_ = false;
};

} // namespace MapleRuntime

#endif // MRT_MS_MARK_WORKER_POLICY_H
