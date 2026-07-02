// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FULL_GC_BARRIER_H
#define MRT_FULL_GC_BARRIER_H

#include <cstdint>
#include <mutex>

#include "Base/Types.h"

namespace MapleRuntime {
class FullGCBarrier {
public:
    static FullGCBarrier& Instance();

    FullGCBarrier(const FullGCBarrier&) = delete;
    FullGCBarrier(FullGCBarrier&&) = delete;
    FullGCBarrier& operator=(const FullGCBarrier&) = delete;
    FullGCBarrier& operator=(FullGCBarrier&&) = delete;

    void Reach(I64 participants);

private:
    FullGCBarrier() = default;

    void WaitForGeneration(std::unique_lock<std::mutex>& lock, uint64_t observedGeneration);
    void Yield();

    std::mutex mutex;
    I64 expectedParticipants = 0;
    I64 arrivedParticipants = 0;
    uint64_t generation = 0;
    bool gcRunning = false;
};
} // namespace MapleRuntime

#endif // MRT_FULL_GC_BARRIER_H
