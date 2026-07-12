// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_WORKER_CONTROL_H
#define MRT_MS_MARK_WORKER_CONTROL_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace MapleRuntime {

class MSMarkWorkerControl {
public:
    enum class Phase {
        STOPPED,
        RUNNING,
        QUIESCING,
    };

    void Start(size_t targetHelpers)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targetHelpers_ = targetHelpers;
        workersInFlight_ = 0;
        assistsInFlight_ = 0;
        phase_ = Phase::RUNNING;
        condition_.notify_all();
    }

    void SetTargetHelpers(size_t targetHelpers)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targetHelpers_ = targetHelpers;
        condition_.notify_all();
    }

    bool WaitUntilEligible(size_t workerId)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this, workerId] {
            return phase_ == Phase::STOPPED ||
                   (phase_ == Phase::RUNNING && workerId < targetHelpers_);
        });
        return phase_ == Phase::RUNNING && workerId < targetHelpers_;
    }

    void BackoffWorker()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait_for(lock, std::chrono::microseconds(50));
    }

    bool TryEnterWorker(size_t workerId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ != Phase::RUNNING || workerId >= targetHelpers_) {
            return false;
        }
        ++workersInFlight_;
        return true;
    }

    void ExitWorker()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --workersInFlight_;
        condition_.notify_all();
    }

    bool TryEnterAssist()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ != Phase::RUNNING) {
            return false;
        }
        ++assistsInFlight_;
        return true;
    }

    void ExitAssist()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --assistsInFlight_;
        condition_.notify_all();
    }

    void BeginQuiesce()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == Phase::RUNNING) {
            phase_ = Phase::QUIESCING;
        }
        condition_.notify_all();
    }

    void WaitForNoInFlight()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return workersInFlight_ == 0 && assistsInFlight_ == 0; });
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = Phase::STOPPED;
        targetHelpers_ = 0;
        condition_.notify_all();
    }

    size_t GetWorkersInFlight() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return workersInFlight_;
    }

    size_t GetAssistsInFlight() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return assistsInFlight_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    Phase phase_ = Phase::STOPPED;
    size_t targetHelpers_ = 0;
    size_t workersInFlight_ = 0;
    size_t assistsInFlight_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_MS_MARK_WORKER_CONTROL_H
