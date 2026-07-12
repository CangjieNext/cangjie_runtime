// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_ASYNC_TASK_STATE_H
#define MRT_ASYNC_TASK_STATE_H

#include <atomic>
#include <cstdint>
#include <limits>

namespace MapleRuntime {

class AsyncTaskState {
public:
    static constexpr uint32_t kInvalidPriority = std::numeric_limits<uint32_t>::max();

    bool Push(uint32_t priority, bool overriding)
    {
        if (priority >= kPriorityBits) {
            return false;
        }
        uint32_t taskMask = 1U << priority;
        uint64_t oldState = state_.load(std::memory_order_acquire);
        while (true) {
            uint32_t pending = Pending(oldState);
            uint32_t running = Running(oldState);
            if (((pending | running) & taskMask) != 0) {
                return false;
            }

            uint32_t newPending = overriding ? taskMask | ((taskMask - 1U) & pending)
                                              : taskMask | pending;
            uint64_t newState = Pack(newPending, running);
            if (state_.compare_exchange_weak(oldState, newState,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return true;
            }
        }
    }

    uint32_t Pop()
    {
        uint64_t oldState = state_.load(std::memory_order_acquire);
        while (true) {
            uint32_t pending = Pending(oldState);
            if (pending == 0) {
                return kInvalidPriority;
            }

            uint32_t priority = static_cast<uint32_t>(__builtin_ctz(pending));
            uint32_t taskMask = 1U << priority;
            uint32_t newPending = pending & (pending - 1U);
            uint32_t newRunning = Running(oldState) | taskMask;
            uint64_t newState = Pack(newPending, newRunning);
            if (state_.compare_exchange_weak(oldState, newState,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return priority;
            }
        }
    }

    void Complete(uint32_t priority)
    {
        if (priority >= kPriorityBits) {
            return;
        }
        uint64_t runningMask = static_cast<uint64_t>(1U << priority) << kPriorityBits;
        state_.fetch_and(~runningMask, std::memory_order_release);
    }

    void Clear()
    {
        state_.store(0, std::memory_order_release);
    }

private:
    static constexpr uint32_t kPriorityBits = std::numeric_limits<uint32_t>::digits;

    static uint32_t Pending(uint64_t state)
    {
        return static_cast<uint32_t>(state);
    }

    static uint32_t Running(uint64_t state)
    {
        return static_cast<uint32_t>(state >> kPriorityBits);
    }

    static uint64_t Pack(uint32_t pending, uint32_t running)
    {
        return static_cast<uint64_t>(pending) |
               (static_cast<uint64_t>(running) << kPriorityBits);
    }

    std::atomic<uint64_t> state_{0};
};

} // namespace MapleRuntime

#endif // MRT_ASYNC_TASK_STATE_H
