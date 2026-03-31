// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Collector/FinalizerProcessor.h"

#include "Base/Macros.h"
#include "Mutator/Mutator.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include <thread>

namespace MapleRuntime {
constexpr U32 DEFAULT_FINALIZER_TIMEOUT_MS = 2000;

// Note: can only be called by FinalizerProcessor thread
extern "C" MRT_EXPORT void* MRT_ProcessFinalizers(void* arg)
{
#ifdef __APPLE__
    CHECK_PTHREAD_CALL(pthread_setname_np, ("finalizer"), "finalizer-processor thread setname");
#elif defined(__linux__) || defined(hongmeng)
    CHECK_PTHREAD_CALL(prctl, (PR_SET_NAME, "finalizer"), "finalizer-processor thread setname");
#endif
    reinterpret_cast<FinalizerProcessor*>(arg)->Run();
    return nullptr;
}

void FinalizerProcessor::Start()
{
    pthread_t thread;
    pthread_attr_t attr;
    size_t stackSize = CangjieRuntime::GetConcurrencyParam().thStackSize * KB; // default 1MB stacksize
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
    // PTHREAD_STACK_MIN is not supported in Windows.
    if (stackSize < PTHREAD_STACK_MIN) {
        stackSize = PTHREAD_STACK_MIN;
    }
#endif
    CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "init pthread attr");
    CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_JOINABLE), "set pthread joinable");
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stackSize), "set pthread stacksize");
    CHECK_PTHREAD_CALL(pthread_create, (&thread, &attr, MRT_ProcessFinalizers, this),
                       "create finalizer-process thread");
#ifdef __WIN64
    CHECK_PTHREAD_CALL(pthread_setname_np, (thread, "finalizer"), "finalizer-processor thread setname");
#endif
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "destroy pthread attr");
    threadHandle = thread;

    WaitStarted();
}

// Stop FinalizerProcessor is only invoked at Fork or Runtime finliazaiton
// Should only invoke once.
void FinalizerProcessor::Stop()
{
    CHECK_DETAIL(running == true, "invalid finalizerProcessor status");
    running = false;
    Notify();
    WaitStop();
}

FinalizerProcessor::FinalizerProcessor()
{
    started = false;
    running = false;
    iterationWaitTime = DEFAULT_FINALIZER_TIMEOUT_MS;
    timeProcessorBegin = 0;
    timeProcessUsed = 0;
    timeCurrentProcessBegin = 0;
    shouldReclaimHeapGarbage.store(false, std::memory_order_relaxed);
    shouldBackgroundSweep_.store(false, std::memory_order_relaxed);
    shouldFeedHungryBuffers.store(false, std::memory_order_relaxed);
}

void FinalizerProcessor::Run()
{
    Init();
    NotifyStarted();
    while (running) {
        if (finalizerWorkList.HasFinalizableJob()) {
            ProcessFinalizables();
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
            LogAfterProcess();
#endif
        }

        if (shouldFeedHungryBuffers.load(std::memory_order_relaxed)) {
            FeedHungryBuffers();
        }

        if (shouldReclaimHeapGarbage.load(std::memory_order_relaxed)) {
            (void)ReclaimHeapGarbage();
        }

        {
            MRT_PHASE_TIMER("finalizerProcessor waitting time", FINALIZE);
            while (running) {
                Wait(iterationWaitTime);
                if (HasPendingWakeWork()) {
                    break;
                }
            }
        }
    }
    Fini();
}

void FinalizerProcessor::Init()
{
    Mutator* mutator = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::FP_THREAD);
    (void)mutator->EnterSaferegion(true);
    tid = mutator->GetTid();
    ThreadLocal::SetProtectAddr(reinterpret_cast<uint8_t*>(0));
    running = true;
    timeProcessorBegin = TimeUtil::MicroSeconds();
    timeProcessUsed = 0;
    MutatorManager::Instance().MutatorManagementRLock();
    fpMutator = mutator;
    MutatorManager::Instance().MutatorManagementRUnlock();
    LOG(RTLOG_INFO, "FinalizerProcessor thread started");
}

void FinalizerProcessor::Fini()
{
    MutatorManager::Instance().MutatorManagementRLock();
    fpMutator = nullptr;
    MutatorManager::Instance().MutatorManagementRUnlock();
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::FP_THREAD);
    LOG(RTLOG_INFO, "FinalizerProcessor thread stopped");
}

void FinalizerProcessor::WaitStop()
{
    pthread_t thread = threadHandle;
    int tmpResult = ::pthread_join(thread, nullptr);
    CHECK_DETAIL(tmpResult == 0, "::pthread_join() in FinalizerProcessor::WaitStop() return %d rather than 0. ",
                 tmpResult);
    started = false;
    threadHandle = 0;
}

void FinalizerProcessor::Notify() { wakeCondition.notify_one(); }

void FinalizerProcessor::Wait(U32 timeoutMilliSeconds)
{
    std::unique_lock<std::mutex> lock(wakeLock);
    std::chrono::milliseconds epoch(timeoutMilliSeconds);
    wakeCondition.wait_for(lock, epoch, [this] { return !running || HasPendingWakeWork(); });
}

void FinalizerProcessor::WaitForActiveBackoff(U32 timeoutMilliSeconds)
{
    std::unique_lock<std::mutex> lock(wakeLock);
    std::chrono::milliseconds epoch(timeoutMilliSeconds);
    wakeCondition.wait_for(lock, epoch);
}

void FinalizerProcessor::NotifyStarted()
{
    {
        std::unique_lock<std::mutex> lock(startedLock);
        CHECK_DETAIL(started != true, "unpexcted true, FinalizerProcessor might not wait stopped");
        started = true;
    }
    startedCondition.notify_all();
}

void FinalizerProcessor::WaitStarted()
{
    std::unique_lock<std::mutex> lock(startedLock);
    if (started) {
        return;
    }
    startedCondition.wait(lock, [this] { return started; });
}

void FinalizerProcessor::ProcessFinalizables()
{
    finalizerWorkList.ProcessFinalizables([this] { return running; });
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void FinalizerProcessor::LogAfterProcess()
{
    if (!ENABLE_LOG(FINALIZE)) {
        return;
    }
    uint64_t timeNow = TimeUtil::MicroSeconds();
    uint64_t timeConsumed = timeNow - timeCurrentProcessBegin;
    uint64_t totalTimePassed = timeNow - timeProcessorBegin;
    timeProcessUsed += timeConsumed;
    constexpr float percentageDivend = 100.0f;
    float percentage = (static_cast<float>(TIME_FACTOR * timeProcessUsed) / totalTimePassed) / percentageDivend;
    DLOG(FINALIZE, "[FinalizerProcessor] End (%luus [%luus] [%.2f%%])", timeConsumed, timeProcessUsed, percentage);
}
#endif

bool FinalizerProcessor::ReclaimHeapGarbage()
{
    ScopedEntryTrace trace("CJRT_GC_RECLAIM");
    size_t releasedBytes = Heap::GetHeap().GetAllocator().ReclaimGarbageMemory(false);
    (void)releasedBytes;
    shouldReclaimHeapGarbage.store(false, std::memory_order_release);
    return false;
}

void FinalizerProcessor::FeedHungryBuffers()
{
    Heap::GetHeap().GetAllocator().FeedHungryBuffers();
    shouldFeedHungryBuffers.store(false, std::memory_order_relaxed);
}

bool FinalizerProcessor::BackgroundSweepLoop()
{
    shouldBackgroundSweep_.store(false, std::memory_order_release);
    return false;
}

void FinalizerProcessor::BackgroundFillAllocCaches()
{
    shouldFeedHungryBuffers.store(false, std::memory_order_relaxed);
}

} // namespace MapleRuntime
