// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "BackgroundProcessor.h"

#include "Base/Macros.h"
#include "Base/TimeUtils.h"
#include "Heap/Heap.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Heap/MAllocator/MSRegionManager.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include <cstdlib>
#include <cstring>
#include <thread>

namespace MapleRuntime {
namespace {
constexpr U32 DEFAULT_MS_GC_HELPER_POLL_MS = 2000;
constexpr U32 DEFAULT_MS_GC_HELPER_ACTIVE_BACKOFF_MS = 1;
constexpr U32 DEFAULT_MS_GC_HELPER_WAKE_BURST_MAX_ROUNDS = 4;
constexpr uint64_t DEFAULT_MS_GC_HELPER_WAKE_BURST_MAX_US = 12000;

bool EqualsIgnoreCase(const char* lhs, const char* rhs)
{
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    while (*lhs != '\0' && *rhs != '\0') {
        char lc = *lhs;
        char rc = *rhs;
        if (lc >= 'A' && lc <= 'Z') {
            lc = static_cast<char>(lc - 'A' + 'a');
        }
        if (rc >= 'A' && rc <= 'Z') {
            rc = static_cast<char>(rc - 'A' + 'a');
        }
        if (lc != rc) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == '\0' && *rhs == '\0';
}

bool IsBackgroundFeedEnabled()
{
    static const bool enabled = []() {
        const char* env = std::getenv("CJ_MS_BG_FEED");
        if (env == nullptr || env[0] == '\0') {
            return true;
        }
        if (std::strcmp(env, "0") == 0 || EqualsIgnoreCase(env, "false") ||
            EqualsIgnoreCase(env, "off") || EqualsIgnoreCase(env, "no")) {
            return false;
        }
        return true;
    }();
    return enabled;
}
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
    preferredFeedCls_.store(kNoPreferredFeedClass, std::memory_order_relaxed);
    sweepPressureMode_.store(false, std::memory_order_relaxed);
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

        if (shouldReclaimHeapGarbage.load(std::memory_order_relaxed)) {
            shouldContinueImmediate = BackgroundReclaimLoop() || shouldContinueImmediate;
        }

        if (shouldFeedHungryBuffers.load(std::memory_order_acquire)) {
            shouldContinueImmediate = BackgroundFeedLoop() || shouldContinueImmediate;
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

void BackgroundProcessor::MaybeNotifyFeed(uint8_t cls)
{
    if (!IsBackgroundFeedEnabled()) {
        shouldFeedHungryBuffers.store(false, std::memory_order_release);
        return;
    }

    // Skip notification when cached regions already cover all active (in-use) regions,
    // or when memory pressure is extreme (feeding is suppressed at that level anyway).
    CentralRegionPool* pool = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator()).GetCentralPool();
    if (pool != nullptr) {
        if (pool->GetFeedPressureLevel() == MSFeedPressureLevel::EXTREME) {
            return;
        }
        size_t cached = pool->GetAllAllocBufferCachedRegionCount();
        size_t active = pool->GetAllAllocBufferActiveRegionCount();
        if (cached >= active) {
            return;
        }
    }
    if (cls != kNoPreferredFeedClass) {
        preferredFeedCls_.store(cls, std::memory_order_release);
    }
    RequestWork(shouldFeedHungryBuffers);
}

void BackgroundProcessor::NotifyToSweep(uint8_t cls)
{
    if (cls != kNoPreferredSweepClass) {
        preferredSweepCls_.store(cls, std::memory_order_release);
    }
    RequestWork(shouldBackgroundSweep_);
}

bool BackgroundProcessor::BackgroundReclaimLoop()
{
    Allocator& allocator = Heap::GetHeap().GetAllocator();
    BackgroundReclaimStats sliceStats = allocator.BackgroundReclaim();
    if (sliceStats.HasWork()) {
        pendingReclaimStats_.Merge(sliceStats);
    }
    bool hasMore = HasPendingReclaimWork();
    if (!hasMore && pendingReclaimStats_.HasWork()) {
        VLOG(REPORT,
             "[MS-GC] reclaim done: slices=%zu regions=%zu bytes=%zuB elapsed=%zuus",
             pendingReclaimStats_.slices,
             pendingReclaimStats_.releasedRegions,
             pendingReclaimStats_.releasedBytes,
             static_cast<size_t>(pendingReclaimStats_.elapsedUs));
        pendingReclaimStats_.Reset();
    }
    return hasMore;
}

bool BackgroundProcessor::HasPendingReclaimWork()
{
    Allocator& allocator = Heap::GetHeap().GetAllocator();
    if (!allocator.HasPendingReclaimWork()) {
        shouldReclaimHeapGarbage.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

bool BackgroundProcessor::BackgroundSweepLoop()
{
    Allocator& allocator = Heap::GetHeap().GetAllocator();
    uint8_t preferredCls = preferredSweepCls_.exchange(kNoPreferredSweepClass, std::memory_order_acq_rel);
    BackgroundSweepStats sliceStats = allocator.BackgroundSweep(preferredCls);
    pendingSweepStats_.Merge(sliceStats);
    bool hasMore = HasPendingSweepWork();
    if (!hasMore && pendingSweepStats_.HasWork()) {
        CentralRegionPool* pool = static_cast<RegionSpaceMS&>(allocator).GetCentralPool();
        size_t remaining = pool ? pool->GetTotalUnsweptCount() : 0;
        VLOG(REPORT,
             "[MS-GC] sweep done: slices=%zu swept=%zu empty=%zu unswept=%zu elapsed=%zuus",
             pendingSweepStats_.slices,
             pendingSweepStats_.sweptRegions,
             pendingSweepStats_.emptyRegions,
             remaining,
             static_cast<size_t>(pendingSweepStats_.elapsedUs));
        pendingSweepStats_.Reset();
    }
    return hasMore;
}

bool BackgroundProcessor::HasPendingSweepWork()
{
    Allocator& allocator = Heap::GetHeap().GetAllocator();
    if (!allocator.HasPendingSweepWork()) {
        // All unswept work done: clear flag and exit pressure mode.
        sweepPressureMode_.store(false, std::memory_order_release);
        shouldBackgroundSweep_.store(false, std::memory_order_release);
        return false;
    }

    // Still has pending work. In pressure mode, keep re-entering the outer
    // Run() loop until all unswept regions are gone.
    if (sweepPressureMode_.load(std::memory_order_acquire)) {
        return true;
    }

    // Normal mode: stop self-rescheduling; wait for next NotifyToSweep.
    shouldBackgroundSweep_.store(false, std::memory_order_release);
    return false;
}

bool BackgroundProcessor::BackgroundFeedLoop()
{
    if (!IsBackgroundFeedEnabled()) {
        shouldFeedHungryBuffers.store(false, std::memory_order_release);
        pendingFeedStats_.Reset();
        return false;
    }

    Allocator& allocator = Heap::GetHeap().GetAllocator();
    uint8_t preferredCls = preferredFeedCls_.exchange(kNoPreferredFeedClass, std::memory_order_acq_rel);
    BackgroundFeedStats sliceStats = allocator.BackgroundFillAllocCaches(preferredCls);
    pendingFeedStats_.Merge(sliceStats);
    bool hasMore = HasPendingFeedWork();
    if (!hasMore && pendingFeedStats_.HasWork()) {
        VLOG(REPORT,
             "[MS-GC] feed done: slices=%zu buffers=%zu classes=%zu regions=%zu pressure=%u elapsed=%zuus",
             pendingFeedStats_.slices,
             pendingFeedStats_.touchedBuffers,
             pendingFeedStats_.filledClasses,
             pendingFeedStats_.filledRegions,
             static_cast<unsigned>(pendingFeedStats_.pressureLevel),
             static_cast<size_t>(pendingFeedStats_.elapsedUs));
        pendingFeedStats_.Reset();
    }
    return hasMore;
}

bool BackgroundProcessor::HasPendingFeedWork()
{
    if (!IsBackgroundFeedEnabled()) {
        shouldFeedHungryBuffers.store(false, std::memory_order_release);
        return false;
    }

    CentralRegionPool* pool = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator()).GetCentralPool();
    if (pool == nullptr) {
        shouldFeedHungryBuffers.store(false, std::memory_order_release);
        return false;
    }
    // When memory pressure is extreme, feeding is suppressed anyway; stop looping.
    if (pool->GetFeedPressureLevel() == MSFeedPressureLevel::EXTREME) {
        shouldFeedHungryBuffers.store(false, std::memory_order_release);
        return false;
    }
    size_t cached = pool->GetAllAllocBufferCachedRegionCount();
    size_t active = pool->GetAllAllocBufferActiveRegionCount();
    if (cached >= active) {
        // All active buffers have a cached spare: feed work done.
        shouldFeedHungryBuffers.store(false, std::memory_order_release);
        return false;
    }
    return true;
}
} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
