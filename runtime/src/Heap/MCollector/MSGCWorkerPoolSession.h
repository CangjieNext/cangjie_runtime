// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MSGC_WORKER_POOL_SESSION_H
#define MRT_MSGC_WORKER_POOL_SESSION_H

#ifdef MRT_USE_MARK_SWEEP

#include "Base/Log.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/HeapWork.h"
#include "Heap/MCollector/MSGCWorkerPoolController.h"
#include <cstddef>
#include <new>

namespace MapleRuntime {

// Owns a fully staged task set until a phase transfers each task to the
// GCThreadPool. The pointer array uses nothrow allocation so a phase can keep
// its documented serial fallback when task staging cannot obtain memory.
class MSGCWorkerTaskBatch {
public:
    explicit MSGCWorkerTaskBatch(size_t capacity)
        : capacity_(capacity), tasks_(capacity == 0
            ? nullptr : new (std::nothrow) HeapWork*[capacity]())
    {
    }

    ~MSGCWorkerTaskBatch()
    {
        Clear();
        delete[] tasks_;
    }

    MSGCWorkerTaskBatch(const MSGCWorkerTaskBatch&) = delete;
    MSGCWorkerTaskBatch& operator=(const MSGCWorkerTaskBatch&) = delete;

    bool IsReady() const
    {
        return capacity_ == 0 || tasks_ != nullptr;
    }

    bool Add(HeapWork* task)
    {
        if (!IsReady() || task == nullptr || count_ == capacity_) {
            return false;
        }
        tasks_[count_++] = task;
        return true;
    }

    HeapWork* Release(size_t index)
    {
        if (index >= count_ || tasks_ == nullptr) {
            return nullptr;
        }
        HeapWork* task = tasks_[index];
        tasks_[index] = nullptr;
        return task;
    }

    size_t GetCount() const
    {
        return count_;
    }

private:
    void Clear()
    {
        for (size_t i = 0; i < count_; ++i) {
            delete tasks_[i];
            tasks_[i] = nullptr;
        }
        count_ = 0;
    }

    size_t capacity_ = 0;
    HeapWork** tasks_ = nullptr;
    size_t count_ = 0;
};

// Owns one short-lived GCThreadPool epoch. It deliberately does not create
// work: each phase retains its own task partitioning and serial fallback. The
// session only centralizes pool ownership, active-width restoration, joining,
// and release on every exit path.
class MSGCWorkerPoolSession {
public:
    MSGCWorkerPoolSession(CollectorResources& resources,
                          MSGCWorkerPoolOwner owner, bool wait)
        : resources_(resources), owner_(owner), wait_(wait)
    {
    }

    ~MSGCWorkerPoolSession()
    {
        Finish();
    }

    MSGCWorkerPoolSession(const MSGCWorkerPoolSession&) = delete;
    MSGCWorkerPoolSession& operator=(const MSGCWorkerPoolSession&) = delete;

    bool Acquire()
    {
        if (acquired_) {
            return true;
        }
        acquired_ = resources_.AcquireMSGCWorkerPool(owner_, wait_);
        threadPool_ = acquired_ ? resources_.GetThreadPool() : nullptr;
        if (threadPool_ == nullptr && acquired_) {
            resources_.ReleaseMSGCWorkerPool(owner_);
            acquired_ = false;
        }
        return threadPool_ != nullptr;
    }

    void AddWork(HeapWork* work)
    {
        CHECK_DETAIL(acquired_ && !started_ && work != nullptr,
                     "invalid MSGC worker-pool task submission");
        threadPool_->AddWork(work);
    }

    void Start(size_t helperCount)
    {
        CHECK_DETAIL(
            acquired_ && !started_ && helperCount != 0,
            "invalid MSGC worker-pool epoch start");
        oldActiveThreadCount_ = threadPool_->GetMaxActiveThreadNum();
        threadPool_->SetMaxActiveThreadNum(static_cast<int32_t>(helperCount));
        threadPool_->Start();
        started_ = true;
    }

    void Finish()
    {
        if (!acquired_) {
            return;
        }
        if (started_) {
            threadPool_->WaitFinish();
            threadPool_->SetMaxActiveThreadNum(oldActiveThreadCount_);
            started_ = false;
        }
        resources_.ReleaseMSGCWorkerPool(owner_);
        threadPool_ = nullptr;
        acquired_ = false;
    }

private:
    CollectorResources& resources_;
    MSGCWorkerPoolOwner owner_;
    bool wait_ = false;
    GCThreadPool* threadPool_ = nullptr;
    int32_t oldActiveThreadCount_ = 0;
    bool acquired_ = false;
    bool started_ = false;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MSGC_WORKER_POOL_SESSION_H
