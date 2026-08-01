// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CollectorResources.h"

#include <algorithm>
#include <thread>

#include "Base/EnvParam.h"
#include "Base/SysCall.h"
#include "CollectorProxy.h"
#include "Common/RunType.h"
#include "Common/ScopedObjectAccess.h"
#include "LoaderManager.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
namespace {
bool IsGCDisabledByEnvironment()
{
    static const bool gcDisabled = ReadBoolParameter("CJ_GC_DISABLE", false);
    return gcDisabled;
}
} // namespace

extern "C" uintptr_t MRT_StopGCWork()
{
    Heap::GetHeap().StopGCWork();
    return 0;
}

void* CollectorResources::GCMainThreadEntry(void* arg)
{
#ifdef __APPLE__
    int ret = pthread_setname_np("gc-main-thread");
    CHECK_E(UNLIKELY(ret != 0), "pthread setname in CollectorResources::StartGCThreads() return %d rather than 0",
            ret);
#elif defined(__linux__) || defined(hongmeng)
    int ret = prctl(PR_SET_NAME, "gc-main-thread");
    CHECK_E(UNLIKELY(ret != 0), "pthread setname in CollectorResources::StartGCThreads() return %d rather than 0",
            ret);
#endif
    
    MRT_ASSERT(arg != nullptr, "GCMainThreadEntry arg=nullptr");
    // set current thread as a gc thread.
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);

    LOG(RTLOG_INFO, "[GC] CollectorResources Thread begin.");

#if defined(__linux__) || defined(hongmeng)
    // set thread priority.
    GCPoolThread::SetThreadPriority(MapleRuntime::GetTid(), GCPoolThread::GC_THREAD_PRIORITY);
#endif

    // run event loop in this thread.
    CollectorResources* collectorResources = reinterpret_cast<CollectorResources*>(arg);
    collectorResources->RunTaskLoop();

    LOG(RTLOG_INFO, "[GC] CollectorResources Thread end.");
    return nullptr;
}

void CollectorResources::Init()
{
    taskQueue = new TaskQueue<GCExecutor>;
    taskQueue->Init();
    finishedGcIndex = GCTask::SYNC_TASK_MIN_INDEX;
    StartGCThreads();
#ifdef MRT_USE_MARK_SWEEP
    backgroundProcessor.Start();
#else
    finalizerProcessor.Start();
#endif
    gcStats.Init();
}

void CollectorResources::Fini()
{
#ifndef MRT_USE_MARK_SWEEP
    MRT_ASSERT(!finalizerProcessor.IsRunning(), "Invalid finalizerProcessor status");
#else
    MRT_ASSERT(!backgroundProcessor.IsRunning(), "Invalid backgroundProcessor status");
#endif
    MRT_ASSERT(!gcThreadRunning.load(std::memory_order_relaxed), "Invalid GC thread status");
    taskQueue->Fini();
    delete taskQueue;
    taskQueue = nullptr;
}

void CollectorResources::StopGCWork()
{
#ifdef MRT_USE_MARK_SWEEP
    msgcWorkerPoolController_.BeginStop();
    backgroundProcessor.Stop();
#else
    finalizerProcessor.Stop();
#endif
    TerminateGCTask();
    StopGCThreads();
}

#ifdef MRT_USE_MARK_SWEEP
bool CollectorResources::TryAcquireMSGCWorkerPoolForSweep()
{
    return gcThreadPool != nullptr && msgcWorkerPoolController_.TryAcquireSweep();
}

bool CollectorResources::TryAcquireMSGCWorkerPoolForPreEnum()
{
    return gcThreadPool != nullptr && msgcWorkerPoolController_.TryAcquirePreEnum();
}

bool CollectorResources::AcquireMSGCWorkerPoolForEnumRoots()
{
    return gcThreadPool != nullptr && msgcWorkerPoolController_.AcquireEnumRoots();
}

bool CollectorResources::AcquireMSGCWorkerPoolForMark()
{
    return gcThreadPool != nullptr && msgcWorkerPoolController_.AcquireMark();
}

void CollectorResources::ReleaseMSGCWorkerPool(MSGCWorkerPoolOwner owner)
{
    bool released = msgcWorkerPoolController_.Release(owner);
    CHECK_DETAIL(released, "invalid MSGC worker-pool owner release: expected=%u actual=%u",
                 static_cast<unsigned>(owner),
                 static_cast<unsigned>(msgcWorkerPoolController_.GetOwner()));
}
#endif

// Send terminate task to gc thread.
void CollectorResources::TerminateGCTask()
{
    if (gcThreadRunning.load(std::memory_order_acquire) == false) {
        return;
    }

    TaskQueue<GCExecutor>::TaskFilter filter = [](GCExecutor&, GCExecutor&) { return false; };
    GCExecutor task(GCTask::TaskType::TASK_TYPE_TERMINATE_GC);
    (void)taskQueue->EnqueueSync(task, filter); // enqueue to sync queue
}

// Usually called from main thread, wait for collector thread to exit.
void CollectorResources::StopGCThreads()
{
    if (gcThreadRunning.load(std::memory_order_acquire) == false) {
        return;
    }
    int ret = ::pthread_join(gcMainThread, nullptr);
    CHECK_E(UNLIKELY(ret != 0), "::pthread_join() in StopGCThreads() return %d", ret);
    // wait the thread pool stopped.
    if (gcThreadPool != nullptr) {
        gcThreadPool->Exit();
        delete gcThreadPool;
        gcThreadPool = nullptr;
    }
    gcThreadRunning.store(false, std::memory_order_release);
}

void CollectorResources::RunTaskLoop()
{
    gcTid.store(MapleRuntime::GetTid(), std::memory_order_release);
    taskQueue->DrainTaskQueue(&collectorProxy);
    NotifyGCFinished(GCTask::TASK_INDEX_FOR_EXIT);
}

// For the ignored gc request, check whether need to wait for current gc finish
void CollectorResources::PostIgnoredGcRequest(GCReason reason)
{
    GCRequest& request = g_gcRequests[reason];
    if (request.IsSyncGC() && isGcStarted.load(std::memory_order_seq_cst)) {
        ScopedEnterSaferegion safeRegion(false);
        WaitForGCFinish();
    }
}

bool CollectorResources::RequestAsyncGC(GCReason reason)
{
    // The gc request must be none blocked
    MRT_ASSERT(!g_gcRequests[reason].IsSyncGC(), "trigger from unsafe context must be none blocked");
    GCExecutor gcTask(GCTask::TaskType::TASK_TYPE_INVOKE_GC, reason);
    // we use async enqueue because this doesn't have locks, lowering the risk
    // of timeouts when entering safe region due to thread scheduling
    return taskQueue->EnqueueAsync(gcTask);
}

AsyncGCEnqueueResult CollectorResources::TryRequestAsyncGC(GCReason reason)
{
    if (IsGCDisabledByEnvironment() || !IsGCActive() || taskQueue == nullptr ||
        !gcThreadRunning.load(std::memory_order_acquire)) {
        return AsyncGCEnqueueResult::GC_INACTIVE_OR_STOPPING;
    }

    GCRequest& request = g_gcRequests[reason];
    MRT_ASSERT(!request.IsSyncGC(), "TryRequestAsyncGC requires an asynchronous GC reason");
    request.SetPrevRequestTime(TimeUtil::NanoSeconds());
    if (collectorProxy.ShouldIgnoreRequest(request)) {
        return AsyncGCEnqueueResult::IGNORED_BY_POLICY;
    }
    return RequestAsyncGC(reason) ? AsyncGCEnqueueResult::ACCEPTED
                                  : AsyncGCEnqueueResult::DUPLICATE_PENDING_OR_RUNNING;
}

void CollectorResources::RequestGCAndWait(GCReason reason)
{
    // Enter saferegion since current thread may blocked by locks.
    ScopedEnterSaferegion enterSaferegion(false);
    GCExecutor gcTask(GCTask::TaskType::TASK_TYPE_INVOKE_GC, reason);

    TaskQueue<GCExecutor>::TaskFilter filter = [](GCExecutor& oldTask, GCExecutor& newTask) {
        return oldTask.GetGCReason() == newTask.GetGCReason();
    };

    GCRequest& request = g_gcRequests[reason];
    // If this gcTask need not to block, just add to async queue
    if (!request.IsSyncGC()) {
        taskQueue->EnqueueAsync(gcTask);
        return;
    }

    // If this gcTask need to block,
    // add gcTask to syncTaskQueue of gcTaskQueue and wait until this gcTask finished
    std::unique_lock<std::mutex> lock(gcFinishedCondMutex);
    uint64_t curThreadSyncIndex = taskQueue->EnqueueSync(gcTask, filter);
    // wait until GC finished
    std::function<bool()> pred = [this, curThreadSyncIndex] {
        return ((finishedGcIndex >= curThreadSyncIndex) || (finishedGcIndex == GCTask::TASK_INDEX_FOR_EXIT));
    };
    gcFinishedCondVar.wait(lock, pred);
}

void CollectorResources::RequestGC(GCReason reason, bool async)
{
    // CJ_GC_DISABLE=1 : suppress all GC requests (for pure-alloc benchmarking).
    if (IsGCDisabledByEnvironment()) {
        return;
    }
    if (!IsGCActive()) {
        return;
    }

    if (async) {
        AsyncGCEnqueueResult result = TryRequestAsyncGC(reason);
        if (result == AsyncGCEnqueueResult::IGNORED_BY_POLICY) {
            DLOG(ALLOC, "ignore gc request");
        }
        return;
    }

    GCRequest& request = g_gcRequests[reason];
    uint64_t curTime = TimeUtil::NanoSeconds();
    request.SetPrevRequestTime(curTime);
    if (collectorProxy.ShouldIgnoreRequest(request)) {
        DLOG(ALLOC, "ignore gc request");
        PostIgnoredGcRequest(reason);
    } else {
        RequestGCAndWait(reason);
    }
}

void CollectorResources::NotifyGCFinished(uint64_t gcIndex)
{
    std::unique_lock<std::mutex> lock(gcFinishedCondMutex);
    isGcStarted.store(false, std::memory_order_release);
#ifdef MRT_USE_MARK_SWEEP
    // PostMark publishes Finalizer work while GC is still active. Re-wake the
    // shared background processor after reopening Finalizer admission so the
    // notification cannot be lost behind the GC-active gate.
    backgroundProcessor.NotifyFinalizerWork();
#endif
    if (gcIndex != GCTask::ASYNC_TASK_INDEX) { // sync gc, need set taskIndex
        finishedGcIndex.store(gcIndex);
    }
    gcFinishedCondVar.notify_all();
}

void CollectorResources::WaitForGCFinish()
{
    uint64_t startTime = TimeUtil::MicroSeconds();
    std::unique_lock<std::mutex> lock(gcFinishedCondMutex);
    uint64_t curWaitGcIndex = finishedGcIndex.load();
    std::function<bool()> pred = [this, curWaitGcIndex] {
        return (!IsGcStarted() || (curWaitGcIndex != finishedGcIndex) ||
                (finishedGcIndex == GCTask::TASK_INDEX_FOR_EXIT));
    };
#ifdef __OHOS__
    std::chrono::seconds waitTime(2); // 2 seconds
    gcFinishedCondVar.wait_for(lock, waitTime, pred);
#else
    gcFinishedCondVar.wait(lock, pred);
#endif
    uint64_t stopTime = TimeUtil::MicroSeconds();
    uint64_t diffTime = stopTime - startTime;
    VLOG(REPORT, "WaitForGCFinish cost %zu us", diffTime);
}

void CollectorResources::StartGCThreads()
{
    bool expected = false;
    if (gcThreadRunning.compare_exchange_strong(expected, true, std::memory_order_acquire) == false) {
        return;
    }
    // starts the thread pool.
    if (gcThreadPool == nullptr) {
#ifdef MRT_USE_MARK_SWEEP
        size_t totalThreadLimit = CangjieRuntime::GetMSGCThreadCapacity();
        gcThreadCount = static_cast<int32_t>(std::min(
            totalThreadLimit,
            static_cast<size_t>(std::max<int32_t>(1, CangjieRuntime::GetGCParam().gcThreads))));
        // cjGCThreads directly selects the GCThreadPool instance width.  The
        // runtime-initialized machine capacity remains its hard upper bound.
        int32_t helperThreads = std::max<int32_t>(0, gcThreadCount - 1);
        VLOG(REPORT,
             "machine GC thread capacity=%zu, GCThreadPool total threads=%d, helper threads=%d",
             totalThreadLimit, gcThreadCount, helperThreads);
#else
        int32_t helperThreads = 1;
        gcThreadCount = helperThreads + 1;
        VLOG(REPORT, "total gc thread count %d, helper thread count %d", gcThreadCount, helperThreads);
#endif
        gcThreadPool = new (std::nothrow) GCThreadPool("gc", helperThreads, GCPoolThread::GC_THREAD_PRIORITY);
        CHECK_DETAIL(gcThreadPool != nullptr, "new GCThreadPool failed");
    }

    // create the collector thread.
    if (::pthread_create(&gcMainThread, nullptr, CollectorResources::GCMainThreadEntry, this) != 0) {
        MRT_ASSERT(0, "pthread_create failed!");
    }
    // set thread name.
#ifdef __WIN64
    int ret = pthread_setname_np(gcMainThread, "gc-main-thread");
    CHECK_E(UNLIKELY(ret != 0), "pthread_setname_np() in CollectorResources::StartGCThreads() return %d rather than 0",
            ret);
#endif
}

int32_t CollectorResources::GetGCThreadCount(const bool isConcurrent) const
{
    if (GetThreadPool() == nullptr) {
        return 1;
    }
    if (isConcurrent) {
        return gcThreadCount;
    }
    // default to 2
    return 2;
}

#ifdef MRT_USE_MARK_SWEEP
size_t CollectorResources::GetMSGCThreadPoolHelperLimit() const
{
    if (GetThreadPool() == nullptr || gcThreadCount <= 1) {
        return 0;
    }
    return std::min(static_cast<size_t>(gcThreadCount - 1),
                    static_cast<size_t>(GetThreadPool()->GetMaxThreadNum()));
}
#endif

void CollectorResources::RequestHeapDump(GCTask::TaskType gcTask)
{
    TaskQueue<GCExecutor>::TaskFilter filter = [](GCExecutor&, GCExecutor&) { return false; };
    GCExecutor dumpTask = GCExecutor(gcTask);
    taskQueue->EnqueueSync(dumpTask, filter);
}

} // namespace MapleRuntime
