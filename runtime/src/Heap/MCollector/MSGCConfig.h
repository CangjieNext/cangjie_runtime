// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_GC_CONFIG_H
#define MRT_MS_GC_CONFIG_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

constexpr uint64_t kMSGCDefaultBackgroundSweepMaxTimeNs = 2ULL * 1000ULL * 1000ULL;
constexpr uint64_t kMSGCDefaultMarkTimeBudgetNs = 10ULL * 1000ULL * 1000ULL * 1000ULL;

enum class MSGCConfigSource : uint8_t { ENVIRONMENT, API };

struct MSGCConfigInput {
    uint64_t backgroundSweepMaxTimeNs = 0;
    uint64_t markTimeBudgetNs = 0;
    size_t assistMaxMultiplier = 0;
    size_t assistSweepMaxRegionsPerCall = 0;
    size_t enumAssistBatchSize = 0;
    uint8_t backgroundSweep = 0;
    uint8_t backgroundScavenge = 0;
    uint8_t backgroundShrink = 0;
    uint8_t verifyMark = 0;
    uint8_t verifyMarkDetail = 0;
    size_t parallelSweepMaxHelpers = 0;
    uint8_t parallelSweep = 0;
    uint8_t adaptiveGrowth = 0;
};

struct MSGCResolvedConfig {
    uint64_t backgroundSweepMaxTimeNs = 0;
    uint64_t markTimeBudgetNs = 0;
    size_t assistBaseRatioPercent = 0;
    size_t assistBacklogStepBuffers = 0;
    size_t assistMaxMultiplier = 0;
    size_t assistCreditDecayRounds = 0;
    size_t assistSweepMaxRegionsPerCall = 0;
    size_t enumAssistBatchSize = 0;
    size_t releasedBusyRetryLimit = 0;
    size_t scavengeRounds = 0;
    uint8_t backgroundSweep = 0;
    uint8_t reservedBackgroundFeed = 0;
    uint8_t backgroundScavenge = 0;
    uint8_t backgroundShrink = 0;
    uint8_t verifyMark = 0;
    uint8_t verifyMarkDetail = 0;
    size_t parallelSweepMaxHelpers = 0;
    uint8_t parallelSweep = 0;
    uint8_t adaptiveGrowth = 0;
};

inline MSGCResolvedConfig ResolveMSGCConfig(const MSGCConfigInput& input,
                                            MSGCConfigSource source)
{
    const bool api = source == MSGCConfigSource::API;
    MSGCResolvedConfig config;
    config.backgroundSweepMaxTimeNs = input.backgroundSweepMaxTimeNs == 0
        ? kMSGCDefaultBackgroundSweepMaxTimeNs : input.backgroundSweepMaxTimeNs;
    config.markTimeBudgetNs = input.markTimeBudgetNs == 0
        ? kMSGCDefaultMarkTimeBudgetNs : input.markTimeBudgetNs;
    config.assistMaxMultiplier = input.assistMaxMultiplier == 0 ? 4 : input.assistMaxMultiplier;
    config.assistSweepMaxRegionsPerCall = input.assistSweepMaxRegionsPerCall == 0
        ? 4 : input.assistSweepMaxRegionsPerCall;
    config.enumAssistBatchSize = input.enumAssistBatchSize == 0 ? 10 : input.enumAssistBatchSize;
    config.backgroundSweep = api && input.backgroundSweep == 0 ? 1 : input.backgroundSweep;
    config.backgroundScavenge = api && input.backgroundScavenge == 0 ? 1 : input.backgroundScavenge;
    config.backgroundShrink = input.backgroundShrink != 0 ? 1 : 0;
    config.verifyMark = input.verifyMark;
    config.verifyMarkDetail = input.verifyMarkDetail;
    config.parallelSweepMaxHelpers = input.parallelSweepMaxHelpers;
    config.parallelSweep = input.parallelSweep != 0 ? 1 : 0;
    config.adaptiveGrowth = input.adaptiveGrowth != 0 ? 1 : 0;
    return config;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_GC_CONFIG_H
