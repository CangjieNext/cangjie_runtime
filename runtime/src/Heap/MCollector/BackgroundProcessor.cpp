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

bool IsBackgroundShrinkEnabled()
{
    static const bool enabled = []() {
        const char* env = std::getenv("CJ_MS_BG_SHRINK");
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
    shouldShrinkHeap_.store(false, std::memory_order_relaxed);
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
    uint64_t lastPeriodicCheckUs = TimeUtil::MicroSeconds();
    while (running.load(std::memory_order_acquire)) {
        uint64_t nowUs = TimeUtil::MicroSeconds();
        if (nowUs - lastPeriodicCheckUs >= static_cast<uint64_t>(iterationWaitTime) * 1000ULL) {
            MaybeSchedulePeriodicWork();
            lastPeriodicCheckUs = nowUs;
        }

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

        if (shouldShrinkHeap_.load(std::memory_order_relaxed)) {
            shouldContinueImmediate = BackgroundShrinkLoop() || shouldContinueImmediate;
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
            if (!HasPendingWakeWork()) {
                // Timeout without explicit notify: return to the outer loop so the
                // periodic inspection path can schedule fallback work uniformly.
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
           shouldScavengeHeap_.load(std::memory_order_relaxed) ||
           shouldShrinkHeap_.load(std::memory_order_relaxed);
}

void BackgroundProcessor::MaybeSchedulePeriodicWork()
{
    if (!shouldBackgroundSweep_.load(std::memory_order_relaxed) && ShouldStartPeriodicSweep()) {
        shouldBackgroundSweep_.store(true, std::memory_order_release);
        preferredSweepCls_.store(kNoPreferredSweepClass, std::memory_order_release);
    }
    if (!shouldScavengeHeap_.load(std::memory_order_relaxed) && ShouldStartPeriodicScavenge()) {
        shouldScavengeHeap_.store(true, std::memory_order_release);
    }
    if (!shouldShrinkHeap_.load(std::memory_order_relaxed) && ShouldStartPeriodicShrink()) {
        shouldShrinkHeap_.store(true, std::memory_order_release);
    }
    if (!shouldFeedHungryBuffers.load(std::memory_order_relaxed) && ShouldStartPeriodicFeed()) {
        shouldFeedHungryBuffers.store(true, std::memory_order_release);
        preferredFeedCls_.store(kNoPreferredFeedClass, std::memory_order_release);
    }
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

bool BackgroundProcessor::ShouldStartPeriodicSweep() const
{
    if (!IsBackgroundSweepEnabled()) {
        return false;
    }
    return Heap::GetHeap().GetAllocator().HasPendingSweepWork();
}

bool BackgroundProcessor::ShouldStartPeriodicScavenge() const
{
    if (!IsBackgroundScavengeEnabled()) {
        return false;
    }
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    return space.HasPendingScavengeWork();
}

bool BackgroundProcessor::ShouldStartPeriodicShrink() const
{
    if (!IsBackgroundShrinkEnabled()) {
        return false;
    }
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    return space.HasPendingShrinkWork();
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

void BackgroundProcessor::NotifyToShrink()
{
    if (!IsBackgroundShrinkEnabled()) {
        shouldShrinkHeap_.store(false, std::memory_order_release);
        return;
    }
    RequestWork(shouldShrinkHeap_);
}

bool BackgroundProcessor::BackgroundSweepLoop()
{
    Allocator& allocator = Heap::GetHeap().GetAllocator();
    uint8_t preferredCls = preferredSweepCls_.exchange(kNoPreferredSweepClass, std::memory_order_acq_rel);
    BackgroundSweepStats sliceStats = allocator.BackgroundSweep(preferredCls);
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(allocator);
    size_t thresholdBytes = space.GetHeapThresholdBytes();
    if (thresholdBytes != 0) {
        (void)space.FlushCentralCacheToThreshold(thresholdBytes);
    }
    pendingSweepStats_.Merge(sliceStats);
    bool hasMore = HasPendingSweepWork();
    if (!hasMore && pendingSweepStats_.HasWork()) {
        CentralRegionPool* pool = space.GetCentralPool();
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

    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(allocator);
    if (space.GetCentralPool() != nullptr) {
        size_t thresholdBytes = space.GetHeapThresholdBytes();
        size_t sweptHeapBytes = space.GetSweptHeapBytes();
        if (thresholdBytes > sweptHeapBytes) {
            return true;
        }
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
    size_t scavengeThreshold = space.GetScavengeThresholdBytes();

    if (!space.HasPendingScavengeWork()) {
        shouldScavengeHeap_.store(false, std::memory_order_release);
        if (pendingScavengeStats_.HasWork()) {
            VLOG(REPORT,
                 "[MS-GC] scavenge done (goal met): slices=%zu regions=%zu bytes=%zuB elapsed=%zuus "
                 "retained=%zuB scavengeThreshold=%zuB",
                 pendingScavengeStats_.slices,
                 pendingScavengeStats_.releasedRegions,
                 pendingScavengeStats_.releasedBytes,
                 static_cast<size_t>(pendingScavengeStats_.elapsedUs),
                 retained, scavengeThreshold);
            pendingScavengeStats_.Reset();
        }
        return false;
    }

    retained = space.GetHeapRetainedBytes();
    scavengeThreshold = space.GetScavengeThresholdBytes();
    size_t targetBytes = (retained > scavengeThreshold) ? (retained - scavengeThreshold) : 0;
    size_t sliceTargetBytes = targetBytes;
    uint64_t t0 = TimeUtil::MicroSeconds();
    size_t released = 0;
    size_t hotFreeBefore = space.GetHotFreeBytes();
    size_t emptySweptBefore = (pool != nullptr) ? pool->GetEmptySweptRegionCount() * MS_REGION_SIZE : 0;
    size_t largeCacheBefore = (pool != nullptr) ? pool->GetLargeHotCacheBytes() : 0;
    if (hotFreeBefore == 0 && emptySweptBefore == 0 && largeCacheBefore == 0) {
        if (pool != nullptr && pool->GetTotalUnsweptCount() > 0) {
            NotifyToSweep(kNoPreferredSweepClass);
            return true;
        }
        shouldScavengeHeap_.store(false, std::memory_order_release);
        return false;
    }

    // On-demand flush to make pages visible to BackgroundScavenge (Step B).
    // Priority: drain the large hot cache FIRST; only touch emptySwept if the large
    // cache alone cannot cover the remaining gap. For each source: if its cached size
    // is <= the gap, flush all of it; otherwise flush only the gap amount.
    {
        size_t hotFreeForGap = space.GetHotFreeBytes();
        size_t gap = (targetBytes > hotFreeForGap) ? (targetBytes - hotFreeForGap) : 0;

        // Priority 1: large hot cache.
        if (gap > 0 && pool != nullptr) {
            size_t largeBytes = pool->GetLargeHotCacheBytes();
            size_t flushed = (largeBytes <= gap)
                ? space.FlushAllLargeHotCache()             // flush all
                : space.FlushLargeHotCacheUpToBytes(gap);   // flush only the gap
            gap = (gap > flushed) ? (gap - flushed) : 0;
        }

        // Priority 2: emptySwept cache, only if the large cache was insufficient.
        if (gap > 0 && space.HasEmptySweptRegions()) {
            size_t emptyBytes = (pool != nullptr) ? pool->GetEmptySweptRegionCount() * MS_REGION_SIZE : 0;
            if (emptyBytes <= gap) {
                (void)space.FlushAllEmptySweptToHotFree();      // flush all
            } else {
                size_t gapRegions = (gap + MS_REGION_SIZE - 1) / MS_REGION_SIZE;
                (void)space.FlushEmptySweptToHotFree(gapRegions); // flush only the gap
            }
        }
    }

    size_t hotFreeAfterFlush = space.GetHotFreeBytes();
    // Snapshot emptySwept AFTER Step A so that any regions flushed into hotFree are
    // already reflected in hotFreeAfterFlush and not double-counted.
    size_t emptySweptAfterFlush = (pool != nullptr) ? pool->GetEmptySweptRegionCount() * MS_REGION_SIZE : 0;
    // Snapshot large cache residual after Step A2 (any flushed bytes are now in releasedTree).
    size_t largeResidualBytes = (pool != nullptr) ? pool->GetLargeHotCacheBytes() : 0;
    if (hotFreeAfterFlush == 0 && emptySweptAfterFlush == 0 && largeResidualBytes == 0) {
        if (pool != nullptr && pool->GetTotalUnsweptCount() > 0) {
            NotifyToSweep(kNoPreferredSweepClass);
            return true;
        }
        shouldScavengeHeap_.store(false, std::memory_order_release);
        return false;
    }

    // Step B: release at most one round of the target. With no mark-window reserve,
    // everything currently available in hotFree is eligible for release.
    size_t scavengeRounds = GetBackgroundScavengeRounds();
    size_t roundTarget = std::max(
        (sliceTargetBytes + scavengeRounds - 1) / scavengeRounds,
        static_cast<size_t>(MS_REGION_SIZE));
    {
        size_t availableToRelease = hotFreeAfterFlush;
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
        NotifyToShrink();
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

bool BackgroundProcessor::HasPendingShrinkWork()
{
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    if (!space.HasPendingShrinkWork()) {
        shouldShrinkHeap_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

bool BackgroundProcessor::BackgroundShrinkLoop()
{
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    uint64_t t0 = TimeUtil::MicroSeconds();
    size_t released = space.BackgroundShrinkOneSegment();
    uint64_t elapsed = TimeUtil::MicroSeconds() - t0;

    if (released > 0) {
        BackgroundReclaimStats slice;
        slice.slices = 1;
        slice.releasedRegions = released / MS_REGION_SIZE;
        slice.releasedBytes = released;
        slice.elapsedUs = elapsed;
        pendingShrinkStats_.Merge(slice);
    }

    bool hasMore = HasPendingShrinkWork();
    if (!hasMore) {
        if (pendingShrinkStats_.HasWork()) {
            VLOG(REPORT,
                 "[MS-GC] shrink done: slices=%zu regions=%zu bytes=%zuB elapsed=%zuus",
                 pendingShrinkStats_.slices,
                 pendingShrinkStats_.releasedRegions,
                 pendingShrinkStats_.releasedBytes,
                 static_cast<size_t>(pendingShrinkStats_.elapsedUs));
            pendingShrinkStats_.Reset();
        }
        shouldShrinkHeap_.store(false, std::memory_order_release);
    }
    return hasMore;
}
} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
