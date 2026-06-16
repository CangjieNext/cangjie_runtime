// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/FullGCBarrier.h"

#include <thread>
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
#include <sched.h>
#endif

#include "Base/Log.h"
#include "Common/ScopedObjectAccess.h"
#include "HeapManager.inline.h"
#include "schedule.h"

namespace MapleRuntime {
FullGCBarrier& FullGCBarrier::Instance()
{
    static FullGCBarrier barrier;
    return barrier;
}

void FullGCBarrier::Yield()
{
    if (CJThreadGetHandle() != nullptr) {
        (void)CJThreadResched();
        return;
    }
#if defined(__linux__) || defined(hongmeng) || defined(__APPLE__)
    (void)sched_yield();
#else
    std::this_thread::yield();
#endif
}

void FullGCBarrier::WaitForGeneration(std::unique_lock<std::mutex>& lock, uint64_t observedGeneration)
{
    while (generation == observedGeneration) {
        lock.unlock();
        Yield();
        lock.lock();
    }
}

void FullGCBarrier::Reach(I64 participants)
{
    CHECK_DETAIL(participants > 0, "reachFullGC participants must be positive: %lld",
                 static_cast<long long>(participants));

    ScopedEnterSaferegion enterSaferegion(false);
    bool shouldTriggerGC = false;
    uint64_t currentGeneration = 0;
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (gcRunning) {
            WaitForGeneration(lock, generation);
        }

        if (expectedParticipants == 0) {
            expectedParticipants = participants;
            arrivedParticipants = 0;
        }

        CHECK_DETAIL(expectedParticipants == participants,
                     "reachFullGC participants mismatch, expected %lld, got %lld",
                     static_cast<long long>(expectedParticipants), static_cast<long long>(participants));

        currentGeneration = generation;
        ++arrivedParticipants;
        CHECK_DETAIL(arrivedParticipants <= expectedParticipants,
                     "reachFullGC arrived participants exceed expected participants");
        if (arrivedParticipants == expectedParticipants) {
            gcRunning = true;
            shouldTriggerGC = true;
        }
    }

    if (shouldTriggerGC) {
        HeapManager::RequestGC(GC_REASON_FULL, true);
        std::lock_guard<std::mutex> lock(mutex);
        expectedParticipants = 0;
        arrivedParticipants = 0;
        gcRunning = false;
        ++generation;
        return;
    }

    std::unique_lock<std::mutex> lock(mutex);
    WaitForGeneration(lock, currentGeneration);
}
} // namespace MapleRuntime
