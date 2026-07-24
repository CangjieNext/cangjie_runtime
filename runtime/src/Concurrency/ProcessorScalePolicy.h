// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_PROCESSOR_SCALE_POLICY_H
#define MRT_PROCESSOR_SCALE_POLICY_H

#include <cstddef>
#include <limits>

namespace MapleRuntime {

inline size_t ResolveProcessorLimit(size_t cpuCount, bool isMarkSweep)
{
    constexpr size_t COPY_GC_PROCESSOR_MULTIPLIER = 2;
    constexpr size_t MARK_SWEEP_PROCESSOR_MULTIPLIER = 20;
    const size_t multiplier = isMarkSweep ? MARK_SWEEP_PROCESSOR_MULTIPLIER : COPY_GC_PROCESSOR_MULTIPLIER;
    if (cpuCount > std::numeric_limits<size_t>::max() / multiplier) {
        return std::numeric_limits<size_t>::max();
    }
    return cpuCount * multiplier;
}

} // namespace MapleRuntime

#endif // MRT_PROCESSOR_SCALE_POLICY_H
