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
constexpr size_t kMSEnumRootsFirstHelperMutators = 1000;
constexpr size_t kMSPostMarkStatsFirstHelperBytes = 2 * 1024 * 1024;
constexpr size_t kMSPostMarkStatsMaxHelpers = 2;

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
    if (configuredThreads != 0) {
        return std::max<size_t>(1, std::min(availableCpus, configuredThreads));
    }

    // Keep the default collector width proportional to both the machine and
    // the Cangjie processor setting.  The total includes the GC main thread,
    // so round each half up to preserve a usable single-thread collector on
    // small configurations.  cjGCThreads remains an explicit override.
    size_t machineDefaultThreads = (availableCpus + 1) / 2;
    size_t processorDefaultThreads = (std::max<size_t>(1, processorCount) + 1) / 2;
    size_t requestedThreads = std::min(machineDefaultThreads, processorDefaultThreads);
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

// EnumRoots fixes its helper width once from the mutator count captured at
// phase entry: [1000, 2000) -> 1, [2000, 4000) -> 2,
// [4000, 8000) -> 3, ... . The width does not track the shrinking pending list.
inline size_t ComputeMSEnumRootsHelperTarget(size_t initialMutatorCount,
                                             size_t maxHelpers)
{
    if (initialMutatorCount < kMSEnumRootsFirstHelperMutators || maxHelpers == 0) {
        return 0;
    }

    size_t target = 1;
    size_t nextBoundary = kMSEnumRootsFirstHelperMutators;
    while (target < maxHelpers &&
           nextBoundary <= std::numeric_limits<size_t>::max() / 2) {
        nextBoundary *= 2;
        if (initialMutatorCount < nextBoundary) {
            break;
        }
        ++target;
    }
    return target;
}

// Marked-statistics scanning is memory-bandwidth bound and runs after mutators
// have resumed. Keep small collect-sets serial and cap the parallel width at
// GC main plus two helpers so statistics do not crowd out application work.
inline size_t ComputeMSPostMarkStatsHelperTarget(size_t bitmapBytes,
                                                 size_t maxHelpers)
{
    if (bitmapBytes < kMSPostMarkStatsFirstHelperBytes || maxHelpers == 0) {
        return 0;
    }
    size_t target = bitmapBytes / kMSPostMarkStatsFirstHelperBytes;
    return std::min({target, maxHelpers, kMSPostMarkStatsMaxHelpers});
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
