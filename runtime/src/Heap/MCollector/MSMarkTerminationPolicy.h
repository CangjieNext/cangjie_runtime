// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_TERMINATION_POLICY_H
#define MRT_MS_MARK_TERMINATION_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace MapleRuntime {

enum class MSMarkTerminationReason : uint8_t {
    NATURAL = 0,
    DEBT_STALLED,
    TIME_BUDGET,
};

constexpr const char* GetMSMarkTerminationReasonName(MSMarkTerminationReason reason)
{
    switch (reason) {
        case MSMarkTerminationReason::NATURAL:
            return "natural";
        case MSMarkTerminationReason::DEBT_STALLED:
            return "debt-stalled";
        case MSMarkTerminationReason::TIME_BUDGET:
            return "time-budget";
    }
    return "unknown";
}

inline uint64_t ScaleMSAssistRatioForPressure(uint64_t baseRatio, size_t multiplier)
{
    if (baseRatio == 0 || multiplier == 0) {
        return 0;
    }
    if (baseRatio > std::numeric_limits<uint64_t>::max() / multiplier) {
        return std::numeric_limits<uint64_t>::max();
    }
    return baseRatio * multiplier;
}

struct MSMarkCheckpointDecision {
    size_t pressureLevel = 0;
    size_t targetHelpers = 0;
    size_t assistMultiplier = 1;
    size_t previousDebt = 0;
    size_t currentDebt = 0;
    size_t noProgressWindows = 0;
    bool debtDecreased = false;
    bool forceForNoProgress = false;

    bool ShouldTerminate() const
    {
        return forceForNoProgress;
    }
};

class MSMarkPressurePolicy {
public:
    static constexpr size_t kCheckpointBuffers = 10;
    static constexpr size_t kBacklogBuffersPerLevel = 10;
    static constexpr size_t kNoProgressWindowsBeforeSTW = 3;

    MSMarkPressurePolicy(size_t maxHelpers, size_t maxAssistMultiplier,
                         bool isOOM, size_t initialDebt)
        : maxHelpers_(maxHelpers),
          maxAssistMultiplier_(std::max<size_t>(1, maxAssistMultiplier)),
          maxPressureLevel_(std::max(maxHelpers_, maxAssistMultiplier_)),
          pressureLevel_(isOOM ? maxPressureLevel_
                               : std::min(BacklogLevel(initialDebt), maxPressureLevel_)),
          previousDebt_(initialDebt)
    {
    }

    static constexpr size_t BacklogLevel(size_t debt)
    {
        return debt == 0 ? 0 : 1 + (debt - 1) / kBacklogBuffersPerLevel;
    }

    MSMarkCheckpointDecision Checkpoint(size_t currentDebt)
    {
        size_t backlogLevel = std::min(BacklogLevel(currentDebt), maxPressureLevel_);
        pressureLevel_ = std::max(pressureLevel_, backlogLevel);

        bool debtDecreased = currentDebt < previousDebt_;
        if (debtDecreased) {
            noProgressWindows_ = 0;
        } else {
            ++noProgressWindows_;
            pressureLevel_ = std::min(pressureLevel_ + 1, maxPressureLevel_);
        }

        MSMarkCheckpointDecision decision {
            pressureLevel_,
            GetTargetHelpers(),
            GetAssistMultiplier(),
            previousDebt_,
            currentDebt,
            noProgressWindows_,
            debtDecreased,
            noProgressWindows_ >= kNoProgressWindowsBeforeSTW
        };
        previousDebt_ = currentDebt;
        return decision;
    }

    size_t GetPressureLevel() const
    {
        return pressureLevel_;
    }

    size_t GetTargetHelpers() const
    {
        return std::min(pressureLevel_, maxHelpers_);
    }

    size_t GetAssistMultiplier() const
    {
        return std::min(std::max<size_t>(1, pressureLevel_), maxAssistMultiplier_);
    }

private:
    size_t maxHelpers_ = 0;
    size_t maxAssistMultiplier_ = 1;
    size_t maxPressureLevel_ = 1;
    size_t pressureLevel_ = 0;
    size_t previousDebt_ = 0;
    size_t noProgressWindows_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_MS_MARK_TERMINATION_POLICY_H
