// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_SHARD_ROUTING_H
#define MRT_MS_SHARD_ROUTING_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstdint>
#include <limits>

namespace MapleRuntime {

constexpr size_t kMSShardSegmentBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint16_t kMSInvalidShardId = std::numeric_limits<uint16_t>::max();

struct MSShardRouteContext {
    uint32_t shardCount = 1;
    bool hasBoundProcessor = false;
    uint32_t processorId = 0;
    uint64_t tid = 0;
};

inline uint32_t NormalizeMSShardCount(uint32_t shardCount)
{
    return shardCount == 0 ? 1 : shardCount;
}

inline size_t ComputeMSInitialCommittedHeapBytes(uint32_t shardCount)
{
    return static_cast<size_t>(NormalizeMSShardCount(shardCount)) * kMSShardSegmentBytes;
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
