// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_BACKGROUND_PROCESSOR_H
#define MRT_BACKGROUND_PROCESSOR_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdint>

#include "Heap/Collector/FinalizerWorkList.h"
#include "Heap/Allocator/Allocator.h"

namespace MapleRuntime {
class Mutator;
class CentralRegionPool;
class RegionSpaceMS;
class CollectorResources;

constexpr uint8_t kNoPreferredSweepClass = UINT8_MAX;

class BackgroundProcessor {
public:
    BackgroundProcessor();
    ~BackgroundProcessor() = default;

    void Start();
    void Stop();
    void Run();
    void Init();
    void Fini();
    void WaitStarted();
    void WaitStop();

    bool IsRunning() const { return running.load(std::memory_order_acquire); }
    uint32_t GetTid() const { return tid; }
    Mutator* GetMutator() const { return bgMutator; }

    U32 VisitFinalizers(const RootVisitor& visitor) { return finalizerWorkList.VisitFinalizers(visitor); }
    void VisitGCRoots(const RootVisitor& visitor) { finalizerWorkList.VisitGCRoots(visitor); }
    void VisitRawPointers(const RootVisitor& visitor) { finalizerWorkList.VisitRawPointers(visitor); }
    void EnqueueFinalizables(const std::function<bool(BaseObject*)>& finalizable, U32 countLimit = UINT_MAX)
    {
        finalizerWorkList.EnqueueFinalizables(finalizable, countLimit);
    }
    void RegisterFinalizer(BaseObject* obj) { finalizerWorkList.RegisterFinalizer(obj); }
    void NotifyFinalizerWork() { Notify(); }

    void NotifyToSweep(uint8_t cls = kNoPreferredSweepClass);
    void NotifyToParallelSweep();
    void NotifyToScavenge();
    void NotifyToShrink();
    size_t GetTargetSweepHelperCount() const
    {
        return targetSweepHelperCount_.load(std::memory_order_acquire);
    }
    size_t GetActiveSweepHelperCount() const
    {
        return activeSweepHelperCount_.load(std::memory_order_acquire);
    }

private:
    void Notify();
    void NotifyStarted();
    void Wait(U32 timeoutMilliSeconds);
    void WaitForActiveBackoff(U32 timeoutMilliSeconds);

    void RequestWork(std::atomic<bool>& flag);
    void MaybeSchedulePeriodicWork();
    bool ShouldStartPeriodicSweep() const;
    bool ShouldStartPeriodicScavenge() const;
    bool ShouldStartPeriodicShrink() const;
    bool HasPendingWakeWork() const;
    bool HasPendingSweepWork();    // check pending sweep work; updates pressure state and shouldBackgroundSweep_
    bool HasPendingShrinkWork();
    size_t RefreshSweepHelperControl(CentralRegionPool& pool, RegionSpaceMS& space,
                                     CollectorResources& resources, bool advanceHelpers);
    void ResetSweepHelperControl(uint32_t sweepGeneration);
    bool BackgroundSweepLoop();
    bool ParallelSweepLoop();
    bool ParallelSweepLoop(size_t helperCount);
    bool BackgroundScavengeLoop(); // scavengeThreshold-based: Step A (flush emptySwept) + Step B (scavenge)
    bool BackgroundShrinkLoop();   // segment-level shrink: release fully idle segments without changing heap topology

    std::mutex wakeLock;
    std::condition_variable wakeCondition;

    std::mutex startedLock;
    std::condition_variable startedCondition;
    std::atomic<bool> started;
    std::atomic<bool> running;

    U32 iterationWaitTime;
    std::atomic<bool> shouldBackgroundSweep_;
    std::atomic<bool> shouldParallelSweep_{false};
    std::atomic<uint64_t> parallelSweepRoundRobin_{0};
    std::atomic<bool> shouldScavengeHeap_{false};
    std::atomic<bool> shouldShrinkHeap_{false};
    std::atomic<uint8_t> preferredSweepCls_{kNoPreferredSweepClass};
    std::atomic<size_t> targetSweepHelperCount_{0};
    std::atomic<size_t> activeSweepHelperCount_{0};
    std::atomic<uint32_t> observedSweepGeneration_{0};
    BackgroundSweepStats pendingSweepStats_{};
    BackgroundReclaimStats pendingScavengeStats_{};
    BackgroundReclaimStats pendingShrinkStats_{};
    FinalizerWorkList finalizerWorkList;
    uint32_t tid = 0;
    pthread_t threadHandle = 0;
    Mutator* bgMutator = nullptr;
};
} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_BACKGROUND_PROCESSOR_H
