// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_WORKER_POLICY_H
#define MRT_MS_MARK_WORKER_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

constexpr size_t kMSMaxTotalMarkThreads = 2;

inline size_t ResolveMSMarkTotalThreads(size_t processorCount, size_t logicalCpuCount,
                                        size_t configuredThreads)
{
    size_t processors = std::max<size_t>(1, processorCount);
    size_t logicalCpus = std::max<size_t>(1, logicalCpuCount);
    size_t availableCpus = std::min(processors, logicalCpus);
    size_t requestedThreads =
        configuredThreads == 0 ? kMSMaxTotalMarkThreads : configuredThreads;
    return std::max<size_t>(
        1, std::min(availableCpus,
                    std::min(kMSMaxTotalMarkThreads, requestedThreads)));
}

class MSMarkWorkerPolicy {
public:
    explicit MSMarkWorkerPolicy(size_t maxHelpers) : maxHelpers_(maxHelpers) {}

    size_t Update(size_t fullBacklog)
    {
        if (maxHelpers_ == 0) {
            return activeHelpers_;
        }

        size_t desiredHelpers = fullBacklog / kBacklogPerHelper;
        if (fullBacklog % kBacklogPerHelper != 0) {
            ++desiredHelpers;
        }
        desiredHelpers = std::max<size_t>(1, std::min(desiredHelpers, maxHelpers_));
        activeHelpers_ = std::max(activeHelpers_, desiredHelpers);
        return activeHelpers_;
    }

    size_t GetActiveHelpers() const { return activeHelpers_; }

private:
    static constexpr size_t kBacklogPerHelper = 8;

    size_t maxHelpers_ = 0;
    size_t activeHelpers_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_MS_MARK_WORKER_POLICY_H
