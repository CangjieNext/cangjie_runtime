// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_WORK_BUF_SCALE_POLICY_H
#define MRT_MS_WORK_BUF_SCALE_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

constexpr size_t kMSWorkBufMinCapacity = 128;
constexpr size_t kMSWorkBufMaxCapacity = 1024;
constexpr size_t kMSWorkBufTargetPrivateObjects = 16 * 1024;
constexpr size_t kMSMarkPressureQuantumObjects = 1ULL << 20;
constexpr size_t kMSAssistCreditDecayReferenceCapacity = 1024;

struct MSWorkBufScaleConfig {
    uint32_t processors = 1;
    size_t capacity = kMSWorkBufMaxCapacity;
    size_t checkpointObjects = kMSMarkPressureQuantumObjects;
    size_t backlogObjectsPerLevel = kMSMarkPressureQuantumObjects;
    size_t assistTriggerObjects = kMSWorkBufMaxCapacity;
    size_t assistSliceObjects = kMSWorkBufMaxCapacity;
    size_t helperPromoteObjects = kMSWorkBufMaxCapacity / 8;
};

inline bool IsValidMSWorkBufCapacity(size_t capacity)
{
    return capacity == 128 || capacity == 256 || capacity == 512 || capacity == 1024;
}

inline size_t FloorMSWorkBufPowerOfTwo(size_t value)
{
    size_t result = 1;
    while (result <= value / 2) {
        result *= 2;
    }
    return result;
}

inline size_t ResolveMSWorkBufCapacity(uint32_t processors, size_t configuredCapacity)
{
    if (configuredCapacity != 0) {
        return configuredCapacity;
    }
    size_t raw = kMSWorkBufTargetPrivateObjects /
                 std::max<size_t>(1, static_cast<size_t>(processors));
    raw = std::max(kMSWorkBufMinCapacity, std::min(kMSWorkBufMaxCapacity, raw));
    return FloorMSWorkBufPowerOfTwo(raw);
}

inline MSWorkBufScaleConfig ResolveMSWorkBufScaleConfig(uint32_t processors,
                                                        size_t configuredCapacity)
{
    size_t capacity = ResolveMSWorkBufCapacity(processors, configuredCapacity);
    MSWorkBufScaleConfig config;
    config.processors = processors;
    config.capacity = capacity;
    config.checkpointObjects = kMSMarkPressureQuantumObjects;
    config.backlogObjectsPerLevel = kMSMarkPressureQuantumObjects;
    config.assistTriggerObjects = capacity;
    config.assistSliceObjects = capacity;
    config.helperPromoteObjects = std::max<size_t>(1, capacity / 8);
    return config;
}

} // namespace MapleRuntime

#endif // MRT_MS_WORK_BUF_SCALE_POLICY_H
