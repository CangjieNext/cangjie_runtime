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
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace MapleRuntime {
namespace {
constexpr U32 DEFAULT_MS_GC_HELPER_POLL_MS = 2000;
constexpr U32 DEFAULT_MS_GC_HELPER_ACTIVE_BACKOFF_MS = 1;
constexpr U32 DEFAULT_MS_GC_HELPER_WAKE_BURST_MAX_ROUNDS = 4;
constexpr uint64_t DEFAULT_MS_GC_HELPER_WAKE_BURST_MAX_US = 12000;
constexpr size_t DEFAULT_MS_FEED_TARGET_CACHED_PER_BUFFER = 5;
constexpr size_t DEFAULT_MS_SCAVENGE_ROUNDS = 8;
constexpr size_t DEFAULT_MS_SCAVENGE_AGGRESSIVE_ROUNDS = 2;
constexpr size_t DEFAULT_MS_SCAVENGE_AGGRESSIVE_THRESHOLD_RATIO = 2;

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

bool IsBackgroundSweepEnabled()
{
    static const bool enabled = []() {
        const char* env = std::getenv("CJ_MS_BG_SWEEP");
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

bool IsBackgroundScavengeEnabled()
{
    static const bool enabled = []() {
        const char* env = std::getenv("CJ_MS_BG_SCAVENGE");
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

size_t GetBackgroundScavengeRounds()
{
    static const size_t rounds = []() {
        const char* env = std::getenv("CJ_MS_SCAVENGE_ROUNDS");
        if (env == nullptr || env[0] == '\0') {
            return DEFAULT_MS_SCAVENGE_ROUNDS;
        }
        if (CString::IsPosNumber(env)) {
            size_t parsed = static_cast<size_t>(std::strtoull(env, nullptr, 10));
            if (parsed > 0) {
                return parsed;
            }
        }
        return DEFAULT_MS_SCAVENGE_ROUNDS;
    }();
    return rounds;
}

size_t GetBackgroundScavengeAggressiveRounds()
{
    static const size_t rounds = []() {
        const char* env = std::getenv("CJ_MS_SCAVENGE_AGGRESSIVE_ROUNDS");
        if (env == nullptr || env[0] == '\0') {
            return DEFAULT_MS_SCAVENGE_AGGRESSIVE_ROUNDS;
        }
        if (CString::IsPosNumber(env)) {
            size_t parsed = static_cast<size_t>(std::strtoull(env, nullptr, 10));
            if (parsed > 0) {
                return parsed;
            }
        }
        return DEFAULT_MS_SCAVENGE_AGGRESSIVE_ROUNDS;
    }();
    return rounds;
}

size_t GetBackgroundScavengeAggressiveRatio()
{
    static const size_t ratio = []() {
        const char* env = std::getenv("CJ_MS_SCAVENGE_AGGRESSIVE_THRESHOLD_RATIO");
        if (env == nullptr || env[0] == '\0') {
            return DEFAULT_MS_SCAVENGE_AGGRESSIVE_THRESHOLD_RATIO;
        }
        if (CString::IsPosNumber(env)) {
            size_t parsed = static_cast<size_t>(std::strtoull(env, nullptr, 10));
            if (parsed > 0) {
                return parsed;
            }
        }
        return DEFAULT_MS_SCAVENGE_AGGRESSIVE_THRESHOLD_RATIO;
    }();
    return ratio;
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
    shouldFeedHungryBuffers.store(false, std::memory_order_relaxed);
    shouldBackgroundSweep_.store(false, std::memory_order_relaxed);
    shouldScavengeHeap_.store(false, std::memory_order_relaxed);
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

        if (shouldScavengeHeap_.load(std::memory_order_relaxed)) {
            shouldContinueImmediate = BackgroundScavengeLoop() || shouldContinueImmediate;
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
            if (!running.load(std::memory_order_acquire)) {
                break;
            }
            if (!HasPendingWakeWork() && ShouldStartPeriodicFeed()) {
                shouldFeedHungryBuffers.store(true, std::memory_order_release);
                preferredFeedCls_.store(kNoPreferredFeedClass, std::memory_order_release);
                break;
            }
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
           shouldFeedHungryBuffers.load(std::memory_order_relaxed) ||
           shouldBackgroundSweep_.load(std::memory_order_relaxed) ||
           shouldScavengeHeap_.load(std::memory_order_relaxed);
}

bool BackgroundProcessor::ShouldStartPeriodicFeed() const
{
    if (!IsBackgroundFeedEnabled()) {
        return false;
    }
    CentralRegionPool* pool = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator()).GetCentralPool();
    if (pool == nullptr) {
        return false;
    }
    size_t bufferCount = pool->GetAllocBufferCount();
    if (bufferCount == 0) {
        return false;
    }
    size_t cachedCount = pool->GetAllAllocBufferCachedRegionCount();
    size_t halfFullThreshold = (bufferCount * DEFAULT_MS_FEED_TARGET_CACHED_PER_BUFFER) / 2;
    if (halfFullThreshold == 0) {
        halfFullThreshold = 1;
    }
    return cachedCount < halfFullThreshold && pool->HasHungryAllocBuffer(kNoPreferredFeedClass);
}

void BackgroundProcessor::NotifyToSweep(uint8_t cls)
{
    if (!IsBackgroundSweepEnabled()) {
        shouldBackgroundSweep_.store(false, std::memory_order_release);
        return;
    }
    if (cls != kNoPreferredSweepClass) {
        preferredSweepCls_.store(cls, std::memory_order_release);
    }
    RequestWork(shouldBackgroundSweep_);
}

void BackgroundProcessor::NotifyToScavenge()
{
    if (!IsBackgroundScavengeEnabled()) {
        shouldScavengeHeap_.store(false, std::memory_order_release);
        return;
    }
    RequestWork(shouldScavengeHeap_);
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
    Allocator& allocator = Heap::GetHeap().GetAllocator();
    uint8_t preferredCls = preferredFeedCls_.exchange(kNoPreferredFeedClass, std::memory_order_acq_rel);
    BackgroundFeedStats sliceStats = allocator.BackgroundFillAllocCaches(preferredCls);
    pendingFeedStats_.Merge(sliceStats);
    bool hasMore = HasPendingFeedWork(preferredCls);
    if (!hasMore && pendingFeedStats_.HasWork()) {
        VLOG(REPORT,
             "[MS-GC] feed done: slices=%zu buffers=%zu classes=%zu regions=%zu elapsed=%zuus",
             pendingFeedStats_.slices,
             pendingFeedStats_.touchedBuffers,
             pendingFeedStats_.filledClasses,
             pendingFeedStats_.filledRegions,
             static_cast<size_t>(pendingFeedStats_.elapsedUs));
        pendingFeedStats_.Reset();
    }
    return hasMore;
}

bool BackgroundProcessor::HasPendingFeedWork(uint8_t preferredCls)
{
    CentralRegionPool* pool = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator()).GetCentralPool();
    if (pool == nullptr) {
        shouldFeedHungryBuffers.store(false, std::memory_order_release);
        return false;
    }
    if (!pool->HasHungryAllocBuffer(preferredCls)) {
        shouldFeedHungryBuffers.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

bool BackgroundProcessor::BackgroundScavengeLoop()
{
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    CentralRegionPool* pool = space.GetCentralPool();            // for large hot cache ops
    size_t retained = space.GetHeapRetainedBytes();
    size_t threshold = space.GetHeapThresholdBytes();
    size_t aggressiveRatio = GetBackgroundScavengeAggressiveRatio();
    bool aggressive = threshold != 0 && retained > threshold * aggressiveRatio;

    if (threshold == 0 || retained <= threshold) {
        shouldScavengeHeap_.store(false, std::memory_order_release);
        if (pendingScavengeStats_.HasWork()) {
            VLOG(REPORT,
                 "[MS-GC] scavenge done (goal met): slices=%zu regions=%zu bytes=%zuB elapsed=%zuus "
                 "retained=%zuB threshold=%zuB",
                 pendingScavengeStats_.slices,
                 pendingScavengeStats_.releasedRegions,
                 pendingScavengeStats_.releasedBytes,
                 static_cast<size_t>(pendingScavengeStats_.elapsedUs),
                 retained, threshold);
            pendingScavengeStats_.Reset();
        }
        return false;
    }

    size_t targetBytes = retained - threshold;
    size_t sliceTargetBytes = targetBytes;
    uint64_t t0 = TimeUtil::MicroSeconds();
    size_t released = 0;

    // Step A (on-demand): if hot-free supply is below the current target and
    // emptySwept cache is non-empty, move half of emptySwept into hot-free first.
    if (space.HasEmptySweptRegions()) {
        size_t hotFree = space.GetHotFreeBytes();
        if (hotFree < targetBytes) {
            if (aggressive) {
                (void)space.FlushAllEmptySweptToHotFree();
            } else {
                (void)space.FlushHalfEmptySweptToHotFree();
            }
        }
    }

    // Step A2 (on-demand): large hot cache → FreeManager (releasedTree).
    // If hot-free is still below target after Step A, drain ~half the large cache.
    // This makes the large cache's pages visible to BackgroundScavenge (Step B).
    if (pool != nullptr && pool->HasLargeFreeCache()) {
        size_t hotFree = space.GetHotFreeBytes();
        if (hotFree < targetBytes) {
            if (aggressive) {
                (void)pool->FlushLargeHotCacheToFreeManager();
            } else {
                (void)pool->FlushHalfLargeFreeCacheToFreeManager();
            }
        }
    }

    size_t hotFreeAfterFlush = space.GetHotFreeBytes();
    // Snapshot emptySwept AFTER Step A so that any regions flushed into hotFree are
    // already reflected in hotFreeAfterFlush and not double-counted.
    size_t emptySweptAfterFlush = space.GetEmptySweptBytes();
    // Snapshot large cache residual after Step A2 (any flushed bytes are now in releasedTree).
    size_t largeResidualBytes = (pool != nullptr) ? pool->GetLargeHotCacheBytes() : 0;

    // Step B: release at most one round of the target.
    // Reserve is satisfied first by emptySwept + large cache residual (pages already
    // managed or staged for release).  Only if that combined reserve < reserveBytes
    // is the shortfall carved from hotFree.
    size_t scavengeRounds = aggressive ? GetBackgroundScavengeAggressiveRounds() : GetBackgroundScavengeRounds();
    size_t roundTarget = std::max(
        (sliceTargetBytes + scavengeRounds - 1) / scavengeRounds,
        static_cast<size_t>(MS_REGION_SIZE));
    {
        const GCStats& gcStats2 = Heap::GetHeap().GetCollector().GetGCStats();
        size_t reserveBytes2 = gcStats2.markWindowReserveBytes;
        // emptySwept + large cache residual satisfy the reserve first.
        size_t combinedReserve = emptySweptAfterFlush + largeResidualBytes;
        size_t hotFreeToKeep = (reserveBytes2 > combinedReserve)
            ? (reserveBytes2 - combinedReserve)
            : 0;
        size_t availableToRelease = (hotFreeAfterFlush > hotFreeToKeep)
            ? (hotFreeAfterFlush - hotFreeToKeep)
            : 0;
        if (availableToRelease == 0) {
            shouldScavengeHeap_.store(false, std::memory_order_release);
            return false;
        }
        size_t quantum = std::min(roundTarget, availableToRelease);
        released = space.BackgroundScavenge(quantum);
    }
    uint64_t elapsed = TimeUtil::MicroSeconds() - t0;

    if (released > 0) {
        BackgroundReclaimStats slice;
        slice.slices = 1;
        slice.releasedRegions = released / MS_REGION_SIZE;
        slice.releasedBytes = released;
        slice.elapsedUs = elapsed;
        pendingScavengeStats_.Merge(slice);
    }

    // Check whether we still have work.
    bool hasMore = space.HasPendingScavengeWork();
    if (!hasMore) {
        // Step C: shrink committed heap end.
        space.BackgroundShrinkHeap();
        if (pendingScavengeStats_.HasWork()) {
            VLOG(REPORT,
                 "[MS-GC] scavenge done: slices=%zu regions=%zu bytes=%zuB elapsed=%zuus",
                 pendingScavengeStats_.slices,
                 pendingScavengeStats_.releasedRegions,
                 pendingScavengeStats_.releasedBytes,
                 static_cast<size_t>(pendingScavengeStats_.elapsedUs));
            pendingScavengeStats_.Reset();
        }
        shouldScavengeHeap_.store(false, std::memory_order_release);
    }
    return hasMore;
}
} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
