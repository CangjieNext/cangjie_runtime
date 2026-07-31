// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MSGC_WORKER_POOL_CONTROLLER_H
#define MRT_MSGC_WORKER_POOL_CONTROLLER_H

#ifdef MRT_USE_MARK_SWEEP

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace MapleRuntime {

enum class MSGCWorkerPoolOwner : uint8_t {
    IDLE,
    SWEEP,
    PRE_ENUM,
    MARK,
    STOPPING,
};

// Serializes complete GCThreadPool task epochs. The controller does not protect
// individual tasks; an owner remains installed from AddWork/Start through
// WaitFinish so PRE_ENUM, MARK, and SWEEP work can never share one pool cycle.
class MSGCWorkerPoolController {
public:
    bool TryAcquireSweep()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || owner_ != MSGCWorkerPoolOwner::IDLE) {
            return false;
        }
        owner_ = MSGCWorkerPoolOwner::SWEEP;
        return true;
    }

    bool TryAcquirePreEnum()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || owner_ != MSGCWorkerPoolOwner::IDLE) {
            return false;
        }
        owner_ = MSGCWorkerPoolOwner::PRE_ENUM;
        return true;
    }

    bool AcquireMark()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
            return stopping_ || owner_ == MSGCWorkerPoolOwner::IDLE;
        });
        if (stopping_) {
            return false;
        }
        owner_ = MSGCWorkerPoolOwner::MARK;
        return true;
    }

    bool Release(MSGCWorkerPoolOwner expectedOwner)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (owner_ != expectedOwner || expectedOwner == MSGCWorkerPoolOwner::IDLE ||
            expectedOwner == MSGCWorkerPoolOwner::STOPPING) {
            return false;
        }
        owner_ = stopping_ ? MSGCWorkerPoolOwner::STOPPING : MSGCWorkerPoolOwner::IDLE;
        condition_.notify_all();
        return true;
    }

    void BeginStop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        if (owner_ == MSGCWorkerPoolOwner::IDLE) {
            owner_ = MSGCWorkerPoolOwner::STOPPING;
        }
        condition_.notify_all();
    }

    MSGCWorkerPoolOwner GetOwner() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return owner_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    MSGCWorkerPoolOwner owner_{MSGCWorkerPoolOwner::IDLE};
    bool stopping_{false};
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MSGC_WORKER_POOL_CONTROLLER_H
