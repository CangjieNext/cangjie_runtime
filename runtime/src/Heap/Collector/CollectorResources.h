// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_RESOURCES_H
#define MRT_COLLECTOR_RESOURCES_H

#include <cstdint>

#include "Base/Macros.h"
#include "FinalizerProcessor.h"
#ifdef MRT_USE_MARK_SWEEP
#include "Heap/MCollector/BackgroundProcessor.h"
#include "Heap/Collector/MSGCWorkerPoolController.h"
#endif
#include "Heap/Collector/TaskQueue.h"
#include "Heap/GcThreadPool.h"
#include "Inspector/CjHeapData.h"
#include "TaskQueue.h"

namespace MapleRuntime {
class CollectorProxy;

enum class AsyncGCEnqueueResult : uint8_t {
    ACCEPTED,
    DUPLICATE_PENDING_OR_RUNNING,
    IGNORED_BY_POLICY,
    GC_INACTIVE_OR_STOPPING,
};

// CollectorResources provides the resources that a functional collector need,
// such as gc thread/threadPool, gc task queue...
class CollectorResources {
public:
    // the collector thread entry routine.
    MRT_EXPORT static void* GCMainThreadEntry(void* arg);

    // a collectorResources without a collector entity is functionless
    explicit CollectorResources(CollectorProxy& proxy) : collectorProxy(proxy) {}
    ATTR_NO_INLINE virtual ~CollectorResources() = default;

    void Init();
    void Fini();
    void StopGCWork();
    void RequestGC(GCReason reason, bool async);
    AsyncGCEnqueueResult TryRequestAsyncGC(GCReason reason);
    void WaitForGCFinish();
    // gc main loop
    void RunTaskLoop();
    // Notify that GC has finished.
    // Must be called by gc thread only
    void NotifyGCFinished(uint64_t gcIndex);
    int32_t GetGCThreadCount(const bool isConcurrent) const;

    GCThreadPool* GetThreadPool() const { return gcThreadPool; }

#ifdef MRT_USE_MARK_SWEEP
    // Helper capacity of the GCThreadPool instance selected by cjGCThreads.
    size_t GetMSGCThreadPoolHelperLimit() const;
    bool AcquireMSGCWorkerPoolForEnumRoots();
    bool AcquireMSGCWorkerPoolForMark();
    // Acquire one complete GCThreadPool task epoch. Short MSGC phases use this
    // through MSGCWorkerPoolSession so ownership, active-width restoration and
    // release stay paired even on a serial fallback path.
    bool AcquireMSGCWorkerPool(MSGCWorkerPoolOwner owner, bool wait);
    void ReleaseMSGCWorkerPool(MSGCWorkerPoolOwner owner);
#endif

    bool IsGcStarted() const { return isGcStarted.load(std::memory_order_acquire); }

    void SetGcStarted(bool val) { isGcStarted.store(val, std::memory_order_release); }

    bool IsGCActive() const { return Heap::GetHeap().IsGCEnabled() && isGCActive.load(std::memory_order_relaxed); }

    FinalizerProcessor& GetFinalizerProcessor() { return finalizerProcessor; }
#ifdef MRT_USE_MARK_SWEEP
    BackgroundProcessor& GetBackgroundProcessor() { return backgroundProcessor; }
#endif
    U32 VisitFinalizers(const RootVisitor& visitor)
    {
#ifdef MRT_USE_MARK_SWEEP
        return backgroundProcessor.VisitFinalizers(visitor);
#else
        return finalizerProcessor.VisitFinalizers(visitor);
#endif
    }
    void VisitFinalizerGCRoots(const RootVisitor& visitor)
    {
#ifdef MRT_USE_MARK_SWEEP
        backgroundProcessor.VisitGCRoots(visitor);
#else
        finalizerProcessor.VisitGCRoots(visitor);
#endif
    }
    U32 EnqueueFinalizables(const std::function<bool(BaseObject*)>& finalizable, U32 countLimit = UINT_MAX)
    {
#ifdef MRT_USE_MARK_SWEEP
        return backgroundProcessor.EnqueueFinalizables(finalizable, countLimit);
#else
        return finalizerProcessor.EnqueueFinalizables(finalizable, countLimit);
#endif
    }
    void RegisterFinalizer(BaseObject* obj)
    {
#ifdef MRT_USE_MARK_SWEEP
        backgroundProcessor.RegisterFinalizer(obj);
#else
        finalizerProcessor.RegisterFinalizer(obj);
#endif
    }
    void NotifyFinalizerWork()
    {
#ifdef MRT_USE_MARK_SWEEP
        backgroundProcessor.NotifyFinalizerWork();
#else
        finalizerProcessor.Notify();
#endif
    }
    GCStats& GetGCStats() { return gcStats; }
    void RequestHeapDump(GCTask::TaskType gcTask);

private:
    void StartGCThreads();
    void TerminateGCTask();
    void StopGCThreads();
    // Notify the GC thread to start GC, and doesn't wait.
    // Called by mutator.
    // reason: The reason for this GC.
    bool RequestAsyncGC(GCReason reason);
    void RequestGCAndWait(GCReason reason);
    void PostIgnoredGcRequest(GCReason reason);

    // the thread pool for parallel tracing.
    GCThreadPool* gcThreadPool = nullptr;
    int32_t gcThreadCount = 1;
    TaskQueue<GCExecutor>* taskQueue = nullptr;

    // the collector thread handle.
    pthread_t gcMainThread = 0;
    std::atomic<pid_t> gcTid{ 0 };
    std::atomic<bool> gcThreadRunning = { false };
    // finishedGcIndex records the currently finished gcIndex
    // may be read by mutator but only be written by gc thread sequentially
    std::atomic<uint64_t> finishedGcIndex = { 0 };
    // protect condition_variable gcFinishedCondVar's status.
    std::mutex gcFinishedCondMutex;
    // notified when GC finished, requires gcFinishedCondMutex
    std::condition_variable gcFinishedCondVar;

    // Indicate whether GC is already started.
    // NOTE: When GC finishes, it clears isGcStarted, must be over-written only by gc thread.
    std::atomic<bool> isGcStarted = { false };

    // a switch to disable gc for hotupdate.
    std::atomic<bool> isGCActive = { true };

    CollectorProxy& collectorProxy;
    FinalizerProcessor finalizerProcessor;
#ifdef MRT_USE_MARK_SWEEP
    mutable BackgroundProcessor backgroundProcessor;
    MSGCWorkerPoolController msgcWorkerPoolController_;
#endif
    GCStats gcStats;
};
} // namespace MapleRuntime
#endif // MRT_COLLECTOR_RESOURCES_H
