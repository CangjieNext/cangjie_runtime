// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MEMORY_LIMIT_POLICY_H
#define MRT_MS_MEMORY_LIMIT_POLICY_H

#ifdef MRT_USE_MARK_SWEEP

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "Heap/MAllocator/MSShardRouting.h"

namespace MapleRuntime {

constexpr size_t kMSSoftHeapLimitMinBytes = 4ULL * 1024ULL * 1024ULL;
constexpr size_t kMSSoftHeapLimitMinKB = kMSSoftHeapLimitMinBytes / 1024ULL;

enum class MSMarkTrigger : uint8_t {
    NONE = 0,
    HEU,
    OOM,
};

inline size_t ResolveMSDefaultVirtualHeapSizeKB(size_t hostMemoryBytes)
{
    size_t boundedBytes = std::min(hostMemoryBytes, kMSMaxVirtualHeapSizeBytes);
    boundedBytes = std::max(boundedBytes, kMSShardSegmentBytes);
    return boundedBytes / 1024ULL;
}

inline size_t ResolveMSSoftHeapLimitKB(size_t hostMemoryBytes, size_t configuredLimitKB)
{
    if (configuredLimitKB != 0) {
        return configuredLimitKB;
    }
    return std::max(hostMemoryBytes / static_cast<size_t>(1024), kMSSoftHeapLimitMinKB);
}

inline size_t ResolveMSScavengeTarget(size_t normalTargetBytes, size_t softLimitBytes)
{
    return softLimitBytes == 0 ? normalTargetBytes : std::min(normalTargetBytes, softLimitBytes);
}

inline size_t ComputeMSReleaseGap(size_t retainedBytes, size_t reclaimableBytes, size_t targetBytes)
{
    size_t releaseGap = retainedBytes > targetBytes ? retainedBytes - targetBytes : 0;
    return std::min(releaseGap, reclaimableBytes);
}

inline MSMarkTrigger ResolveMSMarkTrigger(size_t retainedBytes, size_t sweptBytes,
                                          size_t gcThresholdBytes, size_t softLimitBytes)
{
    if (softLimitBytes != 0 && retainedBytes >= softLimitBytes) {
        return MSMarkTrigger::OOM;
    }
    return sweptBytes >= gcThresholdBytes ? MSMarkTrigger::HEU : MSMarkTrigger::NONE;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_MEMORY_LIMIT_POLICY_H
