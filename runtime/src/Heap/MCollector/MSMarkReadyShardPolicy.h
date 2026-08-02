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

// Probe the caller's home shard first, then a bounded number of remote shards
// starting at the shared rotation cursor.  When the task ledger says work is
// still outstanding, a complete fallback scan prevents sharding from hiding a
// ready token beyond the bounded common path.  The fallback may revisit the
// bounded set; this keeps the common path allocation-free for arbitrary shard
// counts and only affects the already-exceptional full-scan path.
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

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_MARK_READY_SHARD_POLICY_H
