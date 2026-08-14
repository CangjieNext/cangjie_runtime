// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "BackgroundProcessor.h"

#include "Base/Macros.h"
#include "Base/TimeUtils.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/Collector.h"
#include "Heap/MCollector/MSHeapAccounting.h"
#include "Heap/MCollector/MSGCWorkerPoolSession.h"
#include "Heap/Heap.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MAllocator/MSParallelSweepPolicy.h"
#include "Heap/MAllocator/MSRegionManager.h"
#include "Heap/MAllocator/MSRegionSpace.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <thread>

namespace MapleRuntime {
namespace {
constexpr U32 DEFAULT_MS_GC_HELPER_POLL_MS = 2000;
constexpr U32 DEFAULT_MS_GC_HELPER_ACTIVE_BACKOFF_MS = 1;
constexpr U32 DEFAULT_MS_GC_HELPER_WAKE_BURST_MAX_ROUNDS = 4;
constexpr uint64_t DEFAULT_MS_GC_HELPER_WAKE_BURST_MAX_US = 12000;

bool IsBackgroundSweepEnabled()
{
    return CangjieRuntime::GetGCParam().msBackgroundSweep != 0;
}

bool IsParallelSweepEnabled()
{
    const GCParam& param = CangjieRuntime::GetGCParam();
    return param.msBackgroundSweep != 0 && param.msParallelSweep != 0;
}

uint64_t GetSweepBaseBudgetUs()
{
    uint64_t valueNs = CangjieRuntime::GetGCParam().msBackgroundSweepMaxTime;
    return valueNs == 0 ? 0 : std::max<uint64_t>(1, valueNs / 1000ULL);
}

uint64_t SaturatingDeadlineUs(uint64_t startUs, uint64_t budgetUs)
{
    return budgetUs > std::numeric_limits<uint64_t>::max() - startUs
        ? std::numeric_limits<uint64_t>::max()
        : startUs + budgetUs;
}

struct MSSweepSliceWindow {
    uint64_t budgetUs = 0;
    uint64_t deadlineUs = 0;
};

MSSweepSliceWindow BuildMSSweepSliceWindow(SweepPressureMode mode)
{
    uint64_t startUs = TimeUtil::MicroSeconds();
    uint64_t budgetUs = ComputeMSSweepSliceBudgetUs(
        GetSweepBaseBudgetUs(), mode);
    return {budgetUs, SaturatingDeadlineUs(startUs, budgetUs)};
}

bool IsBackgroundScavengeEnabled()
{
    return CangjieRuntime::GetGCParam().msBackgroundScavenge != 0;
}

bool IsBackgroundShrinkEnabled()
{
    return CangjieRuntime::GetGCParam().msBackgroundShrink != 0;
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
    shouldBackgroundSweep_.store(false, std::memory_order_relaxed);
    shouldScavengeHeap_.store(false, std::memory_order_relaxed);
    shouldShrinkHeap_.store(false, std::memory_order_relaxed);
    preferredSweepCls_.store(kNoPreferredSweepClass, std::memory_order_relaxed);
    activeSweepHelperCount_.store(0, std::memory_order_relaxed);
    observedSweepGeneration_.store(0, std::memory_order_relaxed);
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
        if (finalizerWorkList.HasFinalizableJob() &&
            !Heap::GetHeap().GetCollectorResources().IsGcStarted()) {
            finalizerWorkList.ProcessFinalizables([this] {
                return running.load(std::memory_order_acquire) &&
                       !Heap::GetHeap().GetCollectorResources().IsGcStarted();
            });
            shouldContinueImmediate = finalizerWorkList.HasFinalizableJob();
        }

        bool handledParallelSweep = false;
        if (shouldParallelSweep_.load(std::memory_order_relaxed)) {
            shouldContinueImmediate = ParallelSweepLoop() || shouldContinueImmediate;
            handledParallelSweep = true;
        }

        if (!handledParallelSweep && shouldBackgroundSweep_.load(std::memory_order_relaxed)) {
            shouldContinueImmediate = BackgroundSweepLoop() || shouldContinueImmediate;
        }

        if (shouldScavengeHeap_.load(std::memory_order_relaxed)) {
            shouldContinueImmediate = BackgroundScavengeLoop() || shouldContinueImmediate;
        }

        if (shouldShrinkHeap_.load(std::memory_order_relaxed)) {
            shouldContinueImmediate = BackgroundShrinkLoop() || shouldContinueImmediate;
        }

        bool continueParallelSweep = shouldParallelSweep_.load(std::memory_order_acquire) &&
                                     !Heap::GetHeap().IsGcStarted();
        if (continueParallelSweep) {
            // Parallel slices are already time-bounded. Re-enter the outer loop
            // immediately so pressure and helper width are refreshed at the next
            // slice boundary without the generic yield/backoff delay.
            backgroundBurstRounds = 0;
            backgroundBurstStartUs = 0;
            continue;
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

bool BackgroundProcessor::RequestWork(std::atomic<bool>& flag)
{
    if (!flag.exchange(true, std::memory_order_acq_rel)) {
        Notify();
        return true;
    }
    return false;
}

bool BackgroundProcessor::HasPendingWakeWork() const
{
    bool hasRunnableFinalizer = finalizerWorkList.HasFinalizableJob() &&
                                !Heap::GetHeap().GetCollectorResources().IsGcStarted();
    return hasRunnableFinalizer ||
           shouldParallelSweep_.load(std::memory_order_relaxed) ||
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
}

bool BackgroundProcessor::ShouldStartPeriodicSweep() const
{
    if (!IsBackgroundSweepEnabled()) {
        return false;
    }
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    CentralRegionPool* pool = space.GetCentralPool();
    return pool != nullptr && pool->HasPendingSweepWork();
}

bool BackgroundProcessor::ShouldStartPeriodicScavenge() const
{
    if (!IsBackgroundScavengeEnabled()) {
        return false;
    }
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    if (space.GetMSRegionManager() == nullptr) {
        return false;
    }
    MSHeapRetentionSnapshot retention = CaptureMSHeapRetentionSnapshot(space);
    MSHeapPolicySnapshot policy = LoadMSHeapPolicySnapshot(
        Heap::GetHeap().GetCollector().GetGCStats());
    return ComputeMSScavengeReleaseTarget(retention, policy) != 0;
}

bool BackgroundProcessor::ShouldStartPeriodicShrink() const
{
    if (!IsBackgroundShrinkEnabled()) {
        return false;
    }
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    return space.HasPendingShrinkWork();
}

bool BackgroundProcessor::NotifyToSweep(uint8_t cls)
{
    if (!IsBackgroundSweepEnabled()) {
        shouldBackgroundSweep_.store(false, std::memory_order_release);
        return false;
    }
    if (cls != kNoPreferredSweepClass) {
        preferredSweepCls_.store(cls, std::memory_order_release);
    }
    return RequestWork(shouldBackgroundSweep_);
}

bool BackgroundProcessor::NotifyToParallelSweep()
{
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool == nullptr) {
        return false;
    }
    centralPool->ForceHighSweepPressure();
    if (!IsParallelSweepEnabled()) {
        return NotifyToSweep(kNoPreferredSweepClass);
    }
    return RequestWork(shouldParallelSweep_);
}

bool BackgroundProcessor::NotifyToScavenge()
{
    if (!IsBackgroundScavengeEnabled()) {
        shouldScavengeHeap_.store(false, std::memory_order_release);
        return false;
    }
    return RequestWork(shouldScavengeHeap_);
}

void BackgroundProcessor::ResetSweepHelperControl(uint32_t sweepGeneration)
{
    activeSweepHelperCount_.store(0, std::memory_order_release);
    observedSweepGeneration_.store(sweepGeneration, std::memory_order_release);
}

size_t BackgroundProcessor::RefreshSweepHelperControl(CentralRegionPool& pool,
                                                       CollectorResources& resources,
                                                       bool advanceHelpers)
{
    uint32_t sweepGeneration = pool.GetSweepCycleId();
    if (observedSweepGeneration_.load(std::memory_order_acquire) != sweepGeneration) {
        ResetSweepHelperControl(sweepGeneration);
        advanceHelpers = true;
    }
    if (pool.IsSweepDone()) {
        ResetSweepHelperControl(sweepGeneration);
        return 0;
    }

    MSSweepProgressSnapshot sweep = CaptureMSSweepProgressSnapshot(pool);
    MSHeapPolicySnapshot policy = LoadMSHeapPolicySnapshot(
        Heap::GetHeap().GetCollector().GetGCStats());
    size_t sweptBytes = sweep.sweptBytes;
    size_t unsweptBytes = sweep.unsweptBytes;
    size_t thresholdBytes = policy.markThresholdBytes;
    SweepPressureMode pressure = pool.RefreshSweepDebtSnapshot(thresholdBytes, sweptBytes);

    size_t maxHelpers = 0;
    if (IsParallelSweepEnabled()) {
        const GCParam& gcParam = CangjieRuntime::GetGCParam();
        size_t gcThreads = std::min(
            static_cast<size_t>(std::max<int32_t>(1, resources.GetGCThreadCount(true))),
            CangjieRuntime::GetMSGCThreadCapacity());
        maxHelpers = ComputeMSMaxSweepHelpers(gcThreads, gcParam.msParallelSweepMaxHelpers);
    }
    size_t targetHelpers = ComputeMSSweepTargetHelpers(
        pressure, sweptBytes, unsweptBytes, thresholdBytes, maxHelpers);
    size_t currentHelpers = activeSweepHelperCount_.load(std::memory_order_acquire);
    if (advanceHelpers || pressure == SweepPressureMode::HIGH ||
        pool.IsSweepPressureForcedHigh()) {
        currentHelpers = ComputeMSActiveSweepHelpers(
            currentHelpers, targetHelpers, pressure, pool.IsSweepPressureForcedHigh());
    } else {
        currentHelpers = std::min(currentHelpers, maxHelpers);
    }

    activeSweepHelperCount_.store(currentHelpers, std::memory_order_release);
    return currentHelpers;
}

void BackgroundProcessor::BeginSweepRoundLog(CentralRegionPool& pool)
{
    uint32_t epoch = pool.GetSweepCycleId();
    if (sweepRoundLog_.active && sweepRoundLog_.epoch == epoch) {
        return;
    }
    if (sweepRoundLog_.active) {
        FinishSweepRoundLog(nullptr);
    }

    MSSweepProgressSnapshot sweep = CaptureMSSweepProgressSnapshot(pool);
    if (sweep.unsweptRegions == 0) {
        return;
    }
    sweepRoundLog_.active = true;
    sweepRoundLog_.epoch = epoch;
    sweepRoundLog_.startUs = TimeUtil::MicroSeconds();
    sweepRoundLog_.initialUnsweptRegions =
        pool.GetSweepEpochInitialUnsweptRegions();
    sweepRoundLog_.sliceCount = 0;
}

void BackgroundProcessor::FinishSweepRoundLog(CentralRegionPool* pool)
{
    if (!sweepRoundLog_.active) {
        return;
    }
    size_t sweptRegions = sweepRoundLog_.initialUnsweptRegions;
    if (pool != nullptr && pool->GetSweepCycleId() == sweepRoundLog_.epoch) {
        sweptRegions = pool->GetSweepEpochInitialUnsweptRegions();
    }
    VLOG(REPORT,
         "[MS-GC] Sweep round end: epoch=%u sweptRegions=%zu slices=%zu total=%lluus",
         sweepRoundLog_.epoch, sweptRegions, sweepRoundLog_.sliceCount,
         static_cast<unsigned long long>(
             TimeUtil::MicroSeconds() - sweepRoundLog_.startUs));
    sweepRoundLog_ = {};
}

void BackgroundProcessor::BeginScavengeRoundLog(size_t targetBytes)
{
    if (scavengeRoundLog_.active) {
        return;
    }
    scavengeRoundLog_.active = true;
    scavengeRoundLog_.startUs = TimeUtil::MicroSeconds();
    scavengeRoundLog_.targetBytes = targetBytes;
    scavengeRoundLog_.releasedBytes = 0;
    scavengeRoundLog_.sliceCount = 0;
}

void BackgroundProcessor::FinishScavengeRoundLog()
{
    if (!scavengeRoundLog_.active) {
        return;
    }
    VLOG(REPORT,
         "[MS-GC] Scavenge round end: target=%.2fMB released=%.2fMB slices=%zu total=%lluus",
         MSBytesToMB(scavengeRoundLog_.targetBytes),
         MSBytesToMB(scavengeRoundLog_.releasedBytes),
         scavengeRoundLog_.sliceCount,
         static_cast<unsigned long long>(
             TimeUtil::MicroSeconds() - scavengeRoundLog_.startUs));
    scavengeRoundLog_ = {};
}

bool BackgroundProcessor::BackgroundSweepLoop()
{
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    CentralRegionPool* pool = space.GetCentralPool();
    if (pool == nullptr) {
        FinishSweepRoundLog(nullptr);
        shouldBackgroundSweep_.store(false, std::memory_order_release);
        return false;
    }
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    size_t helperCount = RefreshSweepHelperControl(*pool, resources, false);
    if (helperCount != 0 && IsParallelSweepEnabled()) {
        return ParallelSweepLoop(helperCount);
    }

    uint8_t preferredCls = preferredSweepCls_.exchange(
        kNoPreferredSweepClass, std::memory_order_acq_rel);
    SweepPressureMode mode = pool->GetSweepPressureMode();
    MSSweepSliceWindow slice = BuildMSSweepSliceWindow(mode);
    BeginSweepRoundLog(*pool);
    pool->BackgroundSweep(preferredCls, slice.deadlineUs);
    MSHeapPolicySnapshot policy = LoadMSHeapPolicySnapshot(
        Heap::GetHeap().GetCollector().GetGCStats());
    size_t thresholdBytes = policy.markThresholdBytes;
    return CompleteSweepSlice(space, thresholdBytes);
}

bool BackgroundProcessor::ParallelSweepLoop()
{
    shouldParallelSweep_.store(false, std::memory_order_release);
    if (!IsParallelSweepEnabled()) {
        NotifyToSweep(kNoPreferredSweepClass);
        return false;
    }

    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    CentralRegionPool* centralPool = space.GetCentralPool();
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    if (Heap::GetHeap().IsGcStarted()) {
        FinishSweepRoundLog(centralPool);
        ResetSweepHelperControl(centralPool != nullptr ? centralPool->GetSweepCycleId() : 0);
        return false;
    }
    if (centralPool == nullptr) {
        return false;
    }
    size_t helperCount = RefreshSweepHelperControl(*centralPool, resources, false);
    return ParallelSweepLoop(helperCount);
}

bool BackgroundProcessor::ParallelSweepLoop(size_t helperCount)
{
    shouldParallelSweep_.store(false, std::memory_order_release);
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    CentralRegionPool* centralPool = space.GetCentralPool();
    if (centralPool == nullptr || Heap::GetHeap().IsGcStarted()) {
        FinishSweepRoundLog(centralPool);
        ResetSweepHelperControl(centralPool != nullptr ? centralPool->GetSweepCycleId() : 0);
        return false;
    }

    MSSweepProgressSnapshot sweep = CaptureMSSweepProgressSnapshot(*centralPool);
    size_t unsweptRegions = sweep.unsweptRegions;
    if (unsweptRegions == 0) {
        FinishSweepRoundLog(centralPool);
        ResetSweepHelperControl(centralPool->GetSweepCycleId());
        return false;
    }
    SweepPressureMode mode = centralPool->GetSweepPressureMode();
    MSSweepSliceWindow slice = BuildMSSweepSliceWindow(mode);
    MSHeapPolicySnapshot policy = LoadMSHeapPolicySnapshot(
        Heap::GetHeap().GetCollector().GetGCStats());
    size_t sweptBytes = sweep.sweptBytes;
    size_t unsweptBytes = sweep.unsweptBytes;
    size_t thresholdBytes = policy.markThresholdBytes;
    size_t runwayBytes = ComputeMSSweepRunwayBytes(thresholdBytes, sweptBytes);

    (void)unsweptBytes;
    (void)runwayBytes;
    BeginSweepRoundLog(*centralPool);

    uint64_t startClassCursor = parallelSweepRoundRobin_.fetch_add(
        helperCount + 1, std::memory_order_relaxed);
    uint8_t backgroundCls = static_cast<uint8_t>(
        startClassCursor % static_cast<uint64_t>(kNumSizeClasses));
    MSGCWorkerPoolSession sweepSession(
        resources, MSGCWorkerPoolOwner::SWEEP, false);

    if (helperCount == 0 || !sweepSession.Acquire()) {
        if (!Heap::GetHeap().IsGcStarted()) {
            centralPool->BackgroundSweep(backgroundCls, slice.deadlineUs);
        }
    } else {
        if (Heap::GetHeap().IsGcStarted()) {
            sweepSession.Finish();
            ResetSweepHelperControl(centralPool->GetSweepCycleId());
            return false;
        }

        MSGCWorkerTaskBatch stagedTasks(helperCount);
        bool allocationFailed = !stagedTasks.IsReady();
        for (size_t taskIndex = 0; taskIndex < helperCount && !allocationFailed; ++taskIndex) {
            uint8_t preferredCls = static_cast<uint8_t>(
                (startClassCursor + 1 + taskIndex) % static_cast<uint64_t>(kNumSizeClasses));
            HeapWork* task = new (std::nothrow) LambdaWork(
                [centralPool, preferredCls, slice](size_t) {
                    centralPool->ParallelSweep(preferredCls, slice.deadlineUs);
                });
            if (!stagedTasks.Add(task)) {
                delete task;
                allocationFailed = true;
            }
        }

        if (allocationFailed) {
            sweepSession.Finish();
            centralPool->BackgroundSweep(backgroundCls, slice.deadlineUs);
        } else {
            for (size_t taskIndex = 0; taskIndex < stagedTasks.GetCount(); ++taskIndex) {
                HeapWork* task = stagedTasks.Release(taskIndex);
                CHECK(task != nullptr);
                sweepSession.AddWork(task);
            }
            sweepSession.Start(helperCount);
            centralPool->BackgroundSweep(backgroundCls, slice.deadlineUs);
            sweepSession.Finish();
        }
    }

    return CompleteSweepSlice(space, thresholdBytes);
}

bool BackgroundProcessor::CompleteSweepSlice(RegionSpaceMS& space,
                                             size_t thresholdBytes)
{
    if (sweepRoundLog_.active) {
        ++sweepRoundLog_.sliceCount;
    }
    if (thresholdBytes != 0) {
        (void)space.FlushCentralCacheToThreshold(thresholdBytes);
    }
    return RefreshSweepDispatch();
}

bool BackgroundProcessor::RefreshSweepDispatch()
{
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    CentralRegionPool* pool = space.GetCentralPool();
    if (pool == nullptr || !pool->HasPendingSweepWork()) {
        FinishSweepRoundLog(pool);
        ResetSweepHelperControl(pool != nullptr ? pool->GetSweepCycleId() : 0);
        shouldParallelSweep_.store(false, std::memory_order_release);
        shouldBackgroundSweep_.store(false, std::memory_order_release);
        return false;
    }

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    size_t helperCount = RefreshSweepHelperControl(*pool, resources, true);
    bool runParallel = helperCount != 0 && IsParallelSweepEnabled();
    shouldBackgroundSweep_.store(true, std::memory_order_release);
    shouldParallelSweep_.store(runParallel, std::memory_order_release);
    return true;
}

bool BackgroundProcessor::BackgroundScavengeLoop()
{
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    MSRegionManager* regionManager = space.GetMSRegionManager();
    if (regionManager == nullptr) {
        FinishScavengeRoundLog();
        shouldScavengeHeap_.store(false, std::memory_order_release);
        return false;
    }
    MSHeapRetentionSnapshot retention = CaptureMSHeapRetentionSnapshot(space);
    MSHeapPolicySnapshot policy = LoadMSHeapPolicySnapshot(
        Heap::GetHeap().GetCollector().GetGCStats());
    size_t targetBytes = ComputeMSScavengeReleaseTarget(retention, policy);
    if (targetBytes == 0) {
        FinishScavengeRoundLog();
        shouldScavengeHeap_.store(false, std::memory_order_release);
        return false;
    }

    BeginScavengeRoundLog(targetBytes);

    // Keep each background slice bounded to one Segment. FreeManager performs
    // the authoritative per-Region epoch check while claiming candidates.
    size_t segmentBytes = regionManager->GetSegmentSizeBytes();
    size_t quantum = std::min(targetBytes, segmentBytes);
    size_t releasedBytes = space.BackgroundScavenge(quantum);
    scavengeRoundLog_.releasedBytes += releasedBytes;
    ++scavengeRoundLog_.sliceCount;

    // Check whether we still have work.
    // Stop this wake burst when no aged candidate could be claimed. A later
    // Sweep publication or periodic check will retry after more Regions age.
    MSHeapRetentionSnapshot remainingRetention = CaptureMSHeapRetentionSnapshot(space);
    MSHeapPolicySnapshot remainingPolicy = LoadMSHeapPolicySnapshot(
        Heap::GetHeap().GetCollector().GetGCStats());
    bool hasMore = releasedBytes != 0 &&
        ComputeMSScavengeReleaseTarget(remainingRetention, remainingPolicy) != 0;
    if (!hasMore) {
        shouldScavengeHeap_.store(false, std::memory_order_release);
        FinishScavengeRoundLog();
    }
    return hasMore;
}

bool BackgroundProcessor::BackgroundShrinkLoop()
{
    RegionSpaceMS& space = static_cast<RegionSpaceMS&>(Heap::GetHeap().GetAllocator());
    if (!space.HasPendingShrinkWork()) {
        shouldShrinkHeap_.store(false, std::memory_order_release);
        return false;
    }
    (void)space.BackgroundShrinkOneSegment();

    bool hasMore = space.HasPendingShrinkWork();
    if (!hasMore) {
        shouldShrinkHeap_.store(false, std::memory_order_release);
    }
    return hasMore;
}
} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
