// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_WORK_EPOCH_H
#define MRT_WORK_EPOCH_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace MapleRuntime {

// A level-triggered progress notification. Callers capture Load() before
// checking their work predicates, then wait only if those predicates still
// require progress. The epoch predicate catches notifications that race with
// the transition into wait_for(); callers still use a short timeout as the
// upper bound for a delayed notification.
class WorkEpoch {
public:
    uint64_t Load() const
    {
        return epoch_.load(std::memory_order_acquire);
    }

    void Notify()
    {
        epoch_.fetch_add(1, std::memory_order_release);
        if (waiters_.load(std::memory_order_acquire) == 0) {
            return;
        }
        // Pair with the waiter's mutex-protected transition into wait_for().
        // Publishers that observe no waiter remain atomic-only.
        std::lock_guard<std::mutex> lock(mutex_);
        condition_.notify_all();
    }

    template<class Rep, class Period>
    bool WaitForChange(uint64_t observed, const std::chrono::duration<Rep, Period>& timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (epoch_.load(std::memory_order_acquire) != observed) {
            return true;
        }
        waiters_.fetch_add(1, std::memory_order_release);
        bool changed = condition_.wait_for(lock, timeout, [this, observed] {
            return epoch_.load(std::memory_order_acquire) != observed;
        });
        waiters_.fetch_sub(1, std::memory_order_release);
        return changed;
    }

private:
    std::atomic<uint64_t> epoch_{0};
    std::atomic<size_t> waiters_{0};
    std::mutex mutex_;
    std::condition_variable condition_;
};

} // namespace MapleRuntime

#endif // MRT_WORK_EPOCH_H
