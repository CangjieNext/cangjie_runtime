// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
#ifndef MRT_GEN_GC_CONFIG_H
#define MRT_GEN_GC_CONFIG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

struct GenGCConfig {
    size_t youngGenSize = 0;
    size_t survivorRatio = 8;
    size_t oldGenThreshold = 15;
    uint8_t maxAge = 15;
    size_t edenSize = 0;
    size_t survivorSize = 0;
    size_t oldGenSize = 0;

    void Initialize(size_t heapSize)
    {
        youngGenSize = heapSize / 3;
        edenSize = youngGenSize * survivorRatio / (survivorRatio + 2);
        survivorSize = youngGenSize / (survivorRatio + 2);
        oldGenSize = heapSize - youngGenSize;
    }
};

enum class GCType : uint8_t {
    GC_MINOR = 0,
    GC_MAJOR = 1,
    GC_MIXED = 2
};

enum class GCReasonEx : uint8_t {
    REASON_YOUNG_GC = 20,
    REASON_OLD_GC = 21,
    REASON_MIXED_GC = 22,
};

} // namespace MapleRuntime
#endif