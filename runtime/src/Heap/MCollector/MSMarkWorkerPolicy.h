// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_WORKER_POLICY_H
#define MRT_MS_MARK_WORKER_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace MapleRuntime {

constexpr size_t kMSPreEnumResetFirstHelperBytes = 1 * 1024 * 1024;

inline size_t ResolveMSGCThreadCapacity(size_t processorCount,
                                        size_t logicalCpuCount)
{
    // This is the physical GCThreadPool capacity.  It is fixed once at
    // runtime initialization from the machine's logical-core count.  A
    // processor setting is only the fallback on platforms which cannot report
    // that count; cjGCThreads selects the GCThreadPool instance width.
    return logicalCpuCount == 0 ? std::max<size_t>(1, processorCount)
                                : logicalCpuCount;
}

inline size_t ResolveMSMarkTotalThreads(size_t processorCount, size_t logicalCpuCount,
                                        size_t configuredThreads)
{
    size_t availableCpus =
        ResolveMSGCThreadCapacity(processorCount, logicalCpuCount);
    size_t requestedThreads =
        configuredThreads == 0 ? availableCpus : configuredThreads;
    return std::max<size_t>(1, std::min(availableCpus, requestedThreads));
}

// Bitmap reset grows by one helper at each power-of-two reset-size boundary:
// [1MiB, 2MiB) -> 1, [2MiB, 4MiB) -> 2, [4MiB, 8MiB) -> 3, ... .
inline size_t ComputeMSPreEnumResetHelperTarget(size_t resetBytes,
                                                size_t maxHelpers)
{
    if (resetBytes < kMSPreEnumResetFirstHelperBytes || maxHelpers == 0) {
        return 0;
    }

    size_t target = 1;
    size_t nextBoundary = kMSPreEnumResetFirstHelperBytes;
    while (target < maxHelpers &&
           nextBoundary <= std::numeric_limits<size_t>::max() / 2) {
        nextBoundary *= 2;
        if (resetBytes < nextBoundary) {
            break;
        }
        ++target;
    }
    return target;
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
