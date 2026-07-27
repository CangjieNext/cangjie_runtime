// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_SHARD_ROUTING_H
#define MRT_MS_SHARD_ROUTING_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>
#include <limits>

namespace MapleRuntime {

constexpr size_t kMSShardSegmentBytes = 64ULL * 1024ULL * 1024ULL;
constexpr size_t kMSShardSegmentKB = kMSShardSegmentBytes / 1024ULL;
constexpr uint16_t kMSInvalidShardId = std::numeric_limits<uint16_t>::max();
constexpr uint32_t kMSMaxShardCount = static_cast<uint32_t>(kMSInvalidShardId);
constexpr uint32_t kMSLowProcessorDirectShardLimit = 8;
constexpr size_t kMSDefaultInitialSegmentCount = 1;
constexpr size_t kMSRegionsPerShardSegment = 2048;
constexpr size_t kMSMaxUnitsPerRegion = 8;
constexpr size_t kMSUnitsPerShardSegment =
    kMSRegionsPerShardSegment * kMSMaxUnitsPerRegion;
constexpr size_t kMSMaxSegmentCountByAddressSpace =
    (std::numeric_limits<size_t>::max() / 2) / kMSShardSegmentBytes;
constexpr size_t kMSMaxSegmentCountByRegionIndex =
    static_cast<size_t>(std::numeric_limits<uint32_t>::max()) / kMSRegionsPerShardSegment;
constexpr size_t kMSMaxSegmentCountByUnitIndex =
    static_cast<size_t>(std::numeric_limits<uint32_t>::max()) / kMSUnitsPerShardSegment;
constexpr size_t kMSMaxSegmentCountByIndex =
    kMSMaxSegmentCountByRegionIndex < kMSMaxSegmentCountByUnitIndex
        ? kMSMaxSegmentCountByRegionIndex
        : kMSMaxSegmentCountByUnitIndex;
constexpr size_t kMSMaxInitialSegmentCount =
    kMSMaxSegmentCountByAddressSpace < kMSMaxSegmentCountByIndex
        ? kMSMaxSegmentCountByAddressSpace
        : kMSMaxSegmentCountByIndex;
constexpr size_t kMSMaxVirtualHeapSizeBytes =
    kMSMaxInitialSegmentCount * kMSShardSegmentBytes;
constexpr size_t kMSMaxVirtualHeapSizeKB = kMSMaxVirtualHeapSizeBytes / 1024ULL;

struct MSAllocatorScaleConfig {
    uint32_t shardCount = 1;
    uint32_t processorsPerShard = 1;
    size_t initialSegmentCount = kMSDefaultInitialSegmentCount;
};

struct MSShardRouteContext {
    uint32_t shardCount = 1;
    bool hasBoundProcessor = false;
    uint32_t processorId = 0;
    uint64_t tid = 0;
};

inline uint32_t NormalizeMSShardCount(uint32_t shardCount)
{
    if (shardCount == 0) {
        return 1;
    }
    return shardCount > kMSMaxShardCount ? kMSMaxShardCount : shardCount;
}

inline uint32_t NormalizeMSProcessorCount(uint32_t processorCount)
{
    return processorCount == 0 ? 1 : processorCount;
}

inline uint32_t ComputeMSAutomaticShardTarget(uint32_t processorCount)
{
    uint32_t normalizedProcessorCount = NormalizeMSProcessorCount(processorCount);
    if (normalizedProcessorCount <= kMSLowProcessorDirectShardLimit) {
        return normalizedProcessorCount;
    }

    uint64_t scaledProcessorCount = static_cast<uint64_t>(normalizedProcessorCount) * 32ULL;
    uint32_t low = 1;
    uint32_t high = normalizedProcessorCount < kMSMaxShardCount ? normalizedProcessorCount : kMSMaxShardCount;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        uint64_t cube = static_cast<uint64_t>(middle) * middle * middle;
        if (cube >= scaledProcessorCount) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return low;
}

inline uint32_t FindLargestMSShardDivisor(uint32_t processorCount, uint32_t upperBound)
{
    uint32_t normalizedProcessorCount = NormalizeMSProcessorCount(processorCount);
    uint32_t candidate = upperBound;
    if (candidate == 0) {
        candidate = 1;
    }
    if (candidate > normalizedProcessorCount) {
        candidate = normalizedProcessorCount;
    }
    if (candidate > kMSMaxShardCount) {
        candidate = kMSMaxShardCount;
    }
    while (candidate > 1 && normalizedProcessorCount % candidate != 0) {
        --candidate;
    }
    return candidate;
}

inline uint32_t ResolveMSShardCount(uint32_t processorCount, size_t configuredShardCount)
{
    uint32_t normalizedProcessorCount = NormalizeMSProcessorCount(processorCount);
    uint32_t target = 0;
    if (configuredShardCount == 0) {
        target = ComputeMSAutomaticShardTarget(normalizedProcessorCount);
    } else {
        size_t clamped = configuredShardCount;
        if (clamped > normalizedProcessorCount) {
            clamped = normalizedProcessorCount;
        }
        if (clamped > kMSMaxShardCount) {
            clamped = kMSMaxShardCount;
        }
        target = static_cast<uint32_t>(clamped);
    }
    return FindLargestMSShardDivisor(normalizedProcessorCount, target);
}

inline uint32_t ComputeMSProcessorsPerShard(uint32_t processorCount, uint32_t shardCount)
{
    return NormalizeMSProcessorCount(processorCount) / NormalizeMSShardCount(shardCount);
}

inline size_t ResolveMSInitialSegmentCount(size_t configuredSegmentCount)
{
    return configuredSegmentCount == 0 ? kMSDefaultInitialSegmentCount : configuredSegmentCount;
}

inline bool TryResolveMSRequiredVirtualHeapSizeKB(size_t initialSegmentCount,
                                                 size_t configuredMaxVirtualHeapSizeKB,
                                                 size_t& resolvedMaxVirtualHeapSizeKB)
{
    size_t normalizedSegmentCount = ResolveMSInitialSegmentCount(initialSegmentCount);
    if (normalizedSegmentCount > kMSMaxInitialSegmentCount ||
        configuredMaxVirtualHeapSizeKB > kMSMaxVirtualHeapSizeKB) {
        return false;
    }
    size_t requiredVirtualHeapSizeKB = normalizedSegmentCount * kMSShardSegmentKB;
    resolvedMaxVirtualHeapSizeKB = requiredVirtualHeapSizeKB > configuredMaxVirtualHeapSizeKB
        ? requiredVirtualHeapSizeKB
        : configuredMaxVirtualHeapSizeKB;
    return true;
}

inline bool TryComputeMSInitialCommittedHeapBytes(size_t initialSegmentCount, size_t& committedHeapBytes)
{
    size_t normalizedSegmentCount = initialSegmentCount == 0 ? kMSDefaultInitialSegmentCount : initialSegmentCount;
    if (normalizedSegmentCount > kMSMaxInitialSegmentCount) {
        return false;
    }
    committedHeapBytes = normalizedSegmentCount * kMSShardSegmentBytes;
    return true;
}

inline uint16_t ResolveMSHomeShardId(uint16_t shardHint, const MSShardRouteContext& context)
{
    uint32_t normalizedShardCount = NormalizeMSShardCount(context.shardCount);
    if (shardHint != kMSInvalidShardId) {
        return static_cast<uint16_t>(static_cast<uint32_t>(shardHint) % normalizedShardCount);
    }
    if (context.hasBoundProcessor) {
        return static_cast<uint16_t>(context.processorId % normalizedShardCount);
    }
    return static_cast<uint16_t>(static_cast<uint32_t>(context.tid % normalizedShardCount));
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_SHARD_ROUTING_H
