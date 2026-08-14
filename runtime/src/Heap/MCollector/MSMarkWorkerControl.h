// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_WORKER_CONTROL_H
#define MRT_MS_MARK_WORKER_CONTROL_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>

#include "Common/WorkEpoch.h"

namespace MapleRuntime {

class MSMarkWorkerControl {
public:
    enum class Phase {
        STOPPED,
        RUNNING,
        QUIESCING,
    };

    struct Snapshot {
        size_t targetHelpers;
        size_t workersInFlight;
        size_t assistsInFlight;
    };

    explicit MSMarkWorkerControl(WorkEpoch* progressEpoch = nullptr) : progressEpoch_(progressEpoch) {}

    void Start(size_t targetHelpers)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targetHelpers_.store(targetHelpers, std::memory_order_relaxed);
        workersInFlight_.store(0, std::memory_order_relaxed);
        assistsInFlight_.store(0, std::memory_order_relaxed);
        phase_.store(Phase::RUNNING, std::memory_order_release);
        condition_.notify_all();
        NotifyProgress();
    }

    void SetTargetHelpers(size_t targetHelpers)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t oldTarget = targetHelpers_.exchange(targetHelpers, std::memory_order_acq_rel);
        if (oldTarget != targetHelpers) {
            condition_.notify_all();
            NotifyProgress();
        }
    }

    // Assist admission only needs this value. Keep the full snapshot for
    // quiescence diagnostics, where all three counters are semantically used.
    size_t GetTargetHelpers() const
    {
        return targetHelpers_.load(std::memory_order_acquire);
    }

    bool WaitUntilEligible(size_t workerId)
    {
        if (IsEligible(workerId)) {
            return true;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this, workerId] {
            return phase_.load(std::memory_order_acquire) != Phase::RUNNING || IsEligible(workerId);
        });
        return IsEligible(workerId);
    }

    bool TryEnterWorker(size_t workerId)
    {
        if (!IsEligible(workerId)) {
            return false;
        }
        workersInFlight_.fetch_add(1, std::memory_order_acq_rel);
        if (!IsEligible(workerId)) {
            workersInFlight_.fetch_sub(1, std::memory_order_acq_rel);
            NotifyAfterClaimExit();
            return false;
        }
        return true;
    }

    bool CanContinueWorker(size_t workerId) const
    {
        return IsEligible(workerId);
    }

    void ExitWorker()
    {
        workersInFlight_.fetch_sub(1, std::memory_order_acq_rel);
        NotifyAfterClaimExit();
    }

    bool TryEnterAssist()
    {
        if (phase_.load(std::memory_order_acquire) != Phase::RUNNING) {
            return false;
        }
        assistsInFlight_.fetch_add(1, std::memory_order_acq_rel);
        if (phase_.load(std::memory_order_acquire) != Phase::RUNNING) {
            assistsInFlight_.fetch_sub(1, std::memory_order_acq_rel);
            NotifyAfterClaimExit();
            return false;
        }
        return true;
    }

    void ExitAssist()
    {
        assistsInFlight_.fetch_sub(1, std::memory_order_acq_rel);
        NotifyAfterClaimExit();
    }

    void BeginQuiesce()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool changed = false;
        if (phase_.load(std::memory_order_relaxed) == Phase::RUNNING) {
            phase_.store(Phase::QUIESCING, std::memory_order_release);
            changed = true;
        }
        condition_.notify_all();
        if (changed) {
            NotifyProgress();
        }
    }

    void WaitForNoInFlight()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
            return workersInFlight_.load(std::memory_order_acquire) == 0 &&
                   assistsInFlight_.load(std::memory_order_acquire) == 0;
        });
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targetHelpers_.store(0, std::memory_order_relaxed);
        Phase oldPhase = phase_.exchange(Phase::STOPPED, std::memory_order_acq_rel);
        condition_.notify_all();
        if (oldPhase != Phase::STOPPED) {
            NotifyProgress();
        }
    }

    Snapshot GetSnapshot() const
    {
        return Snapshot {
            targetHelpers_.load(std::memory_order_acquire),
            workersInFlight_.load(std::memory_order_acquire),
            assistsInFlight_.load(std::memory_order_acquire),
        };
    }

private:
    bool IsEligible(size_t workerId) const
    {
        return phase_.load(std::memory_order_acquire) == Phase::RUNNING &&
               workerId < targetHelpers_.load(std::memory_order_acquire);
    }

    void NotifyAfterClaimExit()
    {
        bool drained = workersInFlight_.load(std::memory_order_acquire) == 0 &&
                       assistsInFlight_.load(std::memory_order_acquire) == 0;
        if (drained) {
            NotifyProgress();
        }
        // During quiescence every claimant exit wakes the slow-path waiter.
        // Do not rely on a cross-atomic "both zero" snapshot to identify the
        // final exit: either counter may still be observed at an older value.
        if (phase_.load(std::memory_order_acquire) != Phase::RUNNING) {
            std::lock_guard<std::mutex> lock(mutex_);
            condition_.notify_all();
        }
    }

    void NotifyProgress()
    {
        if (progressEpoch_ != nullptr) {
            progressEpoch_->Notify();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<Phase> phase_{Phase::STOPPED};
    std::atomic<size_t> targetHelpers_{0};
    std::atomic<size_t> workersInFlight_{0};
    std::atomic<size_t> assistsInFlight_{0};
    WorkEpoch* progressEpoch_ = nullptr;
};

} // namespace MapleRuntime

#endif // MRT_MS_MARK_WORKER_CONTROL_H
