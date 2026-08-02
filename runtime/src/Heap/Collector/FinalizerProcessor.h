// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_FINALIZER_PROCESSOR_H
#define MRT_FINALIZER_PROCESSOR_H

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "Common/TypeDef.h"
#include "Heap/Collector/FinalizerWorkList.h"

namespace MapleRuntime {
class FinalizerProcessor {
public:
    FinalizerProcessor();
    ~FinalizerProcessor() = default;

    U32 VisitFinalizers(const RootVisitor& visitor) { return finalizerWorkList.VisitFinalizers(visitor); }
    void VisitGCRoots(const RootVisitor& visitor) { finalizerWorkList.VisitGCRoots(visitor); }
    void VisitRawPointers(const RootVisitor& visitor) { finalizerWorkList.VisitRawPointers(visitor); }

    // notify for finalizer processing loop, invoked after GC
    void Notify();
    // wait started flag set, call after create finalizerProcessor thread
    void WaitStarted();

    void Start();
    void Stop();
    void Run();
    void Init();
    void Fini();
    void WaitStop();

    U32 EnqueueFinalizables(const std::function<bool(BaseObject*)>& finalizable, U32 countLimit = UINT_MAX)
    {
        return finalizerWorkList.EnqueueFinalizables(finalizable, countLimit);
    }
    void RegisterFinalizer(BaseObject* obj) { finalizerWorkList.RegisterFinalizer(obj); }
    bool IsRunning() const { return running; }
    uint32_t GetTid() const { return tid; }

    Mutator* GetMutator() const { return fpMutator; }

    void NotifyToReclaimGarbage()
    {
        RequestWork(shouldReclaimHeapGarbage);
    }
    void NotifyToFeedAllocBuffers()
    {
        RequestWork(shouldFeedHungryBuffers);
    }
private:
    void RequestWork(std::atomic<bool>& flag)
    {
        if (!flag.exchange(true, std::memory_order_acq_rel)) {
            Notify();
        }
    }

    bool HasPendingWakeWork() const
    {
        return finalizerWorkList.HasFinalizableJob() ||
               shouldReclaimHeapGarbage.load(std::memory_order_relaxed) ||
               shouldFeedHungryBuffers.load(std::memory_order_relaxed);
    }

    void NotifyStarted();
    void Wait(U32 timeoutMilliSeconds);
    void WaitForActiveBackoff(U32 timeoutMilliSeconds);
    void ProcessFinalizables();
    bool ReclaimHeapGarbage();
    void FeedHungryBuffers();

    std::mutex wakeLock;
    std::condition_variable wakeCondition; // notify finalizer processing continue

    std::mutex startedLock;
    std::condition_variable startedCondition; // notify finalizerProcessor thread is started
    volatile bool started;

    volatile bool running; // Initially false and set true after finalizerProcessor thread start, set false when stop

    U32 iterationWaitTime;

    FinalizerWorkList finalizerWorkList;
    std::atomic<bool> shouldReclaimHeapGarbage;
    std::atomic<bool> shouldFeedHungryBuffers;
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    // stats
    void LogAfterProcess();
#endif
    uint64_t timeProcessorBegin;
    uint64_t timeProcessUsed;
    uint64_t timeCurrentProcessBegin;
    uint32_t tid = 0;
    pthread_t threadHandle = 0; // thread handle to thread
    Mutator* fpMutator = nullptr;
};
} // namespace MapleRuntime
#endif // MRT_FINALIZER_PROCESSOR_H
