// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "BackgroundProcessor.h"

#include "Base/Macros.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include <thread>

namespace MapleRuntime {
namespace {
constexpr U32 DEFAULT_MS_GC_HELPER_POLL_MS = 2000;
constexpr U32 DEFAULT_MS_GC_HELPER_ACTIVE_BACKOFF_MS = 1;
constexpr U32 DEFAULT_MS_GC_HELPER_WAKE_BURST_MAX_ROUNDS = 4;
constexpr uint64_t DEFAULT_MS_GC_HELPER_WAKE_BURST_MAX_US = 12000;
}

extern "C" MRT_EXPORT void* MRT_ProcessBackgroundWork(void* arg)
{
#ifdef __APPLE__
    CHECK_PTHREAD_CALL(pthread_setname_np, ("gc-helper"), "msgc-background thread setname");
#elif defined(__linux__) || defined(hongmeng)
    CHECK_PTHREAD_CALL(prctl, (PR_SET_NAME, "gc-helper"), "msgc-background thread setname");
#endif
    reinterpret_cast<BackgroundProcessor*>(arg)->Run();
    return nullptr;
}

BackgroundProcessor::BackgroundProcessor()
{
    started.store(false, std::memory_order_relaxed);
    running.store(false, std::memory_order_relaxed);
    iterationWaitTime = DEFAULT_MS_GC_HELPER_POLL_MS;
    shouldReclaimHeapGarbage.store(false, std::memory_order_relaxed);
    shouldFeedHungryBuffers.store(false, std::memory_order_relaxed);
    shouldBackgroundSweep_.store(false, std::memory_order_relaxed);
    preferredSweepCls_.store(kNoPreferredSweepClass, std::memory_order_relaxed);
}

void BackgroundProcessor::Start()
{
    pthread_t thread;
    pthread_attr_t attr;
    size_t stackSize = CangjieRuntime::GetConcurrencyParam().thStackSize * KB;
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
    if (stackSize < PTHREAD_STACK_MIN) {
        stackSize = PTHREAD_STACK_MIN;
    }
#endif
    CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), "init pthread attr");
    CHECK_PTHREAD_CALL(pthread_attr_setdetachstate, (&attr, PTHREAD_CREATE_JOINABLE), "set pthread joinable");
    CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stackSize), "set pthread stacksize");
    CHECK_PTHREAD_CALL(pthread_create, (&thread, &attr, MRT_ProcessBackgroundWork, this),
                       "create msgc-background thread");
#ifdef __WIN64
    CHECK_PTHREAD_CALL(pthread_setname_np, (thread, "gc-helper"), "msgc-background thread setname");
#endif
    CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), "destroy pthread attr");
    threadHandle = thread;
    WaitStarted();
}

void BackgroundProcessor::Stop()
{
    CHECK_DETAIL(running.load(std::memory_order_acquire), "invalid backgroundProcessor status");
    running.store(false, std::memory_order_release);
    Notify();
    WaitStop();
}

void BackgroundProcessor::Run()
{
    Init();
    NotifyStarted();
    uint32_t backgroundBurstRounds = 0;
    uint64_t backgroundBurstStartUs = 0;
    while (running.load(std::memory_order_acquire)) {
        bool shouldContinueImmediate = false;
        if (finalizerWorkList.HasFinalizableJob()) {
            finalizerWorkList.ProcessFinalizables([this] { return running.load(std::memory_order_acquire); });
            shouldContinueImmediate = finalizerWorkList.HasFinalizableJob();
        }

        if (shouldBackgroundSweep_.load(std::memory_order_relaxed)) {
            shouldContinueImmediate = BackgroundSweepLoop() || shouldContinueImmediate;
        }

        if (shouldFeedHungryBuffers.load(std::memory_order_acquire)) {
            BackgroundFillAllocCaches();
        }

        if (shouldContinueImmediate) {
            if (backgroundBurstRounds == 0) {
                backgroundBurstStartUs = TimeUtil::MicroSeconds();
            }
            ++backgroundBurstRounds;

            uint64_t burstElapsedUs = TimeUtil::MicroSeconds() - backgroundBurstStartUs;
            if (backgroundBurstRounds < DEFAULT_MS_GC_HELPER_WAKE_BURST_MAX_ROUNDS &&
                burstElapsedUs < DEFAULT_MS_GC_HELPER_WAKE_BURST_MAX_US) {
                std::this_thread::yield();
                continue;
            }

            backgroundBurstRounds = 0;
            backgroundBurstStartUs = 0;
            WaitForActiveBackoff(DEFAULT_MS_GC_HELPER_ACTIVE_BACKOFF_MS);
            continue;
        }

        if (shouldReclaimHeapGarbage.load(std::memory_order_relaxed)) {
            shouldContinueImmediate = ReclaimHeapGarbage() || shouldContinueImmediate;
        }

        backgroundBurstRounds = 0;
        backgroundBurstStartUs = 0;
        while (running.load(std::memory_order_acquire)) {
            if (HasPendingWakeWork()) {
                break;
            }
            Wait(iterationWaitTime);
        }
    }
    Fini();
}

void BackgroundProcessor::Init()
{
    Mutator* mutator = MutatorManager::Instance().CreateRuntimeMutator(ThreadType::MSGC_BACKGROUND_THREAD);
    (void)mutator->EnterSaferegion(true);
    tid = mutator->GetTid();
    ThreadLocal::SetProtectAddr(reinterpret_cast<uint8_t*>(0));
    running.store(true, std::memory_order_release);
    MutatorManager::Instance().MutatorManagementRLock();
    bgMutator = mutator;
    MutatorManager::Instance().MutatorManagementRUnlock();
    LOG(RTLOG_INFO, "BackgroundProcessor thread started");
}

void BackgroundProcessor::Fini()
{
    MutatorManager::Instance().MutatorManagementRLock();
    bgMutator = nullptr;
    MutatorManager::Instance().MutatorManagementRUnlock();
    MutatorManager::Instance().DestroyRuntimeMutator(ThreadType::MSGC_BACKGROUND_THREAD);
    LOG(RTLOG_INFO, "BackgroundProcessor thread stopped");
}

void BackgroundProcessor::WaitStarted()
{
    std::unique_lock<std::mutex> lock(startedLock);
    if (started.load(std::memory_order_acquire)) {
        return;
    }
    startedCondition.wait(lock, [this] { return started.load(std::memory_order_acquire); });
}

void BackgroundProcessor::WaitStop()
{
    pthread_t thread = threadHandle;
    int tmpResult = ::pthread_join(thread, nullptr);
    CHECK_DETAIL(tmpResult == 0,
                 "::pthread_join() in BackgroundProcessor::WaitStop() return %d rather than 0. ",
                 tmpResult);
    started.store(false, std::memory_order_release);
    threadHandle = 0;
}

void BackgroundProcessor::Notify() { wakeCondition.notify_one(); }

void BackgroundProcessor::NotifyStarted()
{
    {
        std::unique_lock<std::mutex> lock(startedLock);
        CHECK_DETAIL(!started.load(std::memory_order_relaxed),
                     "unpexcted true, BackgroundProcessor might not wait stopped");
        started.store(true, std::memory_order_release);
    }
    startedCondition.notify_all();
}

void BackgroundProcessor::Wait(U32 timeoutMilliSeconds)
{
    std::unique_lock<std::mutex> lock(wakeLock);
    std::chrono::milliseconds epoch(timeoutMilliSeconds);
    wakeCondition.wait_for(lock, epoch,
                           [this] { return !running.load(std::memory_order_acquire) || HasPendingWakeWork(); });
}

void BackgroundProcessor::WaitForActiveBackoff(U32 timeoutMilliSeconds)
{
    std::unique_lock<std::mutex> lock(wakeLock);
    std::chrono::milliseconds epoch(timeoutMilliSeconds);
    wakeCondition.wait_for(lock, epoch);
}

void BackgroundProcessor::RequestWork(std::atomic<bool>& flag)
{
    if (!flag.exchange(true, std::memory_order_acq_rel)) {
        Notify();
    }
}

bool BackgroundProcessor::HasPendingWakeWork() const
{
    return finalizerWorkList.HasFinalizableJob() ||
           shouldReclaimHeapGarbage.load(std::memory_order_relaxed) ||
           shouldFeedHungryBuffers.load(std::memory_order_relaxed) ||
           shouldBackgroundSweep_.load(std::memory_order_relaxed);
}

void BackgroundProcessor::NotifyToReclaimGarbage()
{
    RequestWork(shouldReclaimHeapGarbage);
}

void BackgroundProcessor::NotifyToFeedAllocBuffers()
{
    RequestWork(shouldFeedHungryBuffers);
}

void BackgroundProcessor::NotifyToSweep(uint8_t cls)
{
    if (cls != kNoPreferredSweepClass) {
        preferredSweepCls_.store(cls, std::memory_order_release);
    }
    RequestWork(shouldBackgroundSweep_);
}

bool BackgroundProcessor::ReclaimHeapGarbage()
{
    ScopedEntryTrace trace("CJRT_GC_RECLAIM");
    Allocator& allocator = Heap::GetHeap().GetAllocator();
    size_t releasedBytes = allocator.ReclaimGarbageMemory(false);
    bool pendingReclaim = allocator.HasPendingReclaimWork();
    shouldReclaimHeapGarbage.store(pendingReclaim, std::memory_order_release);
    return pendingReclaim;
}

bool BackgroundProcessor::BackgroundSweepLoop()
{
    Allocator& allocator = Heap::GetHeap().GetAllocator();
    uint8_t preferredCls = preferredSweepCls_.exchange(kNoPreferredSweepClass, std::memory_order_acq_rel);
    allocator.BackgroundSweep(preferredCls);
    bool pendingSweep = allocator.HasPendingSweepWork();
    shouldBackgroundSweep_.store(pendingSweep, std::memory_order_release);
    if (!pendingSweep && allocator.HasPendingReclaimWork()) {
        shouldReclaimHeapGarbage.store(true, std::memory_order_release);
    }
    return pendingSweep;
}

void BackgroundProcessor::BackgroundFillAllocCaches()
{
    shouldFeedHungryBuffers.store(false, std::memory_order_release);
    Heap::GetHeap().GetAllocator().BackgroundFillAllocCaches();
}
} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
