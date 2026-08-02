// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_READY_SHARD_POLICY_H
#define MRT_MS_MARK_READY_SHARD_POLICY_H

#ifdef MRT_USE_MARK_SWEEP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace MapleRuntime {

constexpr size_t kMSMarkReadyLimitedRemoteProbes = 4;
constexpr size_t kMSMarkReadyAssistRemoteProbes = 2;

// P threads publish mutator work, while the GC main and every configured Mark
// helper publish child work.  Half as many ready shards retain useful sharing
// without returning every participant to one contended tagged head.
inline size_t ComputeMSMarkReadyShardCount(
    size_t processorCount, size_t totalGCMarkThreads)
{
    size_t totalParticipants;
    if (processorCount >
        std::numeric_limits<size_t>::max() - totalGCMarkThreads) {
        totalParticipants = std::numeric_limits<size_t>::max();
    } else {
        totalParticipants = processorCount + totalGCMarkThreads;
    }
    if (totalParticipants == 0) {
        return 1;
    }
    return totalParticipants / 2 + totalParticipants % 2;
}

inline size_t ResolveMSMarkReadyPreferredShard(
    size_t participantId, size_t shardCount)
{
    return shardCount == 0 ? 0 : participantId % shardCount;
}

// GC main and helpers retain the complete scheduler lookup. The common path
// starts at the shared rotation cursor; a non-zero ledger enables a complete
// fallback so GC-owned workers do not sleep while ready work is merely hidden
// in another shard.
template<typename TryShard>
bool TryMSMarkReadyShardProbe(
    size_t shardCount, size_t preferredShard, size_t rotationStart,
    bool fullFallback, TryShard&& tryShard)
{
    if (shardCount == 0) {
        return false;
    }

    preferredShard %= shardCount;
    if (tryShard(preferredShard)) {
        return true;
    }
    if (shardCount == 1) {
        return false;
    }

    rotationStart %= shardCount;
    size_t remoteLimit = std::min(
        kMSMarkReadyLimitedRemoteProbes, shardCount - 1);
    size_t remoteProbes = 0;
    for (size_t offset = 0;
         offset < shardCount && remoteProbes < remoteLimit; ++offset) {
        size_t shard = (rotationStart + offset) % shardCount;
        if (shard == preferredShard) {
            continue;
        }
        ++remoteProbes;
        if (tryShard(shard)) {
            return true;
        }
    }

    if (!fullFallback) {
        return false;
    }
    for (size_t offset = 0; offset < shardCount; ++offset) {
        size_t shard = (rotationStart + offset) % shardCount;
        if (shard == preferredShard) {
            continue;
        }
        if (tryShard(shard)) {
            return true;
        }
    }
    return false;
}

// Assist avoids the shared rotation cursor and full fallback scan. It probes
// its home shard and at most two remote shards from owner-local state. A later
// Assist call resumes at the following shard, so misses remain cheap without
// permanently hiding work.
template<typename TryShard>
bool TryMSMarkReadyAssistShardProbe(
    size_t shardCount, size_t preferredShard, size_t& localCursor,
    TryShard&& tryShard)
{
    if (shardCount == 0) {
        return false;
    }

    preferredShard %= shardCount;
    if (tryShard(preferredShard)) {
        return true;
    }
    if (shardCount == 1) {
        return false;
    }

    size_t remoteStart = localCursor % shardCount;
    size_t remoteLimit = std::min(
        kMSMarkReadyAssistRemoteProbes, shardCount - 1);
    size_t remoteProbes = 0;
    for (size_t offset = 0;
         offset < shardCount && remoteProbes < remoteLimit; ++offset) {
        size_t shard = (remoteStart + offset) % shardCount;
        if (shard == preferredShard) {
            continue;
        }
        ++remoteProbes;
        localCursor = (shard + 1) % shardCount;
        if (tryShard(shard)) {
            return true;
        }
    }
    return false;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_MARK_READY_SHARD_POLICY_H
