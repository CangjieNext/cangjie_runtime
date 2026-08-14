#include <atomic>
#include <cstdlib>
#include <thread>

#define MRT_USE_MARK_SWEEP 1
#include "src/Heap/MCollector/MSGCWorkerPoolController.h"

namespace {

using MapleRuntime::MSGCWorkerPoolController;
using MapleRuntime::MSGCWorkerPoolOwner;

void TestSweepIsNonBlockingAndExclusive()
{
    MSGCWorkerPoolController controller;
    if (!controller.Acquire(MSGCWorkerPoolOwner::SWEEP, false)) {
        std::abort();
    }
    if (controller.Acquire(MSGCWorkerPoolOwner::SWEEP, false)) {
        std::abort();
    }
    if (controller.Release(MSGCWorkerPoolOwner::MARK)) {
        std::abort();
    }
    if (!controller.Release(MSGCWorkerPoolOwner::SWEEP)) {
        std::abort();
    }
    if (!controller.Acquire(MSGCWorkerPoolOwner::MARK, false) ||
        !controller.Release(MSGCWorkerPoolOwner::MARK)) {
        std::abort();
    }
}

void TestMarkWaitsForSweepHandoff()
{
    MSGCWorkerPoolController controller;
    if (!controller.Acquire(MSGCWorkerPoolOwner::SWEEP, false)) {
        std::abort();
    }

    std::atomic<bool> waiterStarted{false};
    std::atomic<bool> markAcquired{false};
    std::thread waiter([&] {
        waiterStarted.store(true, std::memory_order_release);
        if (controller.Acquire(MSGCWorkerPoolOwner::MARK, true)) {
            markAcquired.store(true, std::memory_order_release);
            if (!controller.Release(MSGCWorkerPoolOwner::MARK)) {
                std::abort();
            }
        }
    });

    while (!waiterStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    for (size_t i = 0; i < 100; ++i) {
        std::this_thread::yield();
    }
    if (markAcquired.load(std::memory_order_acquire)) {
        std::abort();
    }

    if (!controller.Release(MSGCWorkerPoolOwner::SWEEP)) {
        std::abort();
    }
    waiter.join();
    if (!markAcquired.load(std::memory_order_acquire)) {
        std::abort();
    }
    if (!controller.Acquire(MSGCWorkerPoolOwner::SWEEP, false) ||
        !controller.Release(MSGCWorkerPoolOwner::SWEEP)) {
        std::abort();
    }
}

void TestStoppingRejectsNewOwners()
{
    MSGCWorkerPoolController controller;
    controller.BeginStop();
    if (controller.Acquire(MSGCWorkerPoolOwner::SWEEP, false) ||
        controller.Acquire(MSGCWorkerPoolOwner::MARK, true)) {
        std::abort();
    }
}

} // namespace

int main()
{
    TestSweepIsNonBlockingAndExclusive();
    TestMarkWaitsForSweepHandoff();
    TestStoppingRejectsNewOwners();
    return 0;
}
