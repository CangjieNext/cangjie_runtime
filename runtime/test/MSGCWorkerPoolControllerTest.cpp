#include <atomic>
#include <cstdlib>
#include <thread>

#define MRT_USE_MARK_SWEEP 1
#include "src/Heap/Collector/MSGCWorkerPoolController.h"

namespace {

using MapleRuntime::MSGCWorkerPoolController;
using MapleRuntime::MSGCWorkerPoolOwner;

void TestSweepIsNonBlockingAndExclusive()
{
    MSGCWorkerPoolController controller;
    if (!controller.TryAcquireSweep()) {
        std::abort();
    }
    if (controller.TryAcquireSweep()) {
        std::abort();
    }
    if (controller.Release(MSGCWorkerPoolOwner::MARK)) {
        std::abort();
    }
    if (controller.GetOwner() != MSGCWorkerPoolOwner::SWEEP) {
        std::abort();
    }
    if (!controller.Release(MSGCWorkerPoolOwner::SWEEP)) {
        std::abort();
    }
}

void TestMarkWaitsForSweepHandoff()
{
    MSGCWorkerPoolController controller;
    if (!controller.TryAcquireSweep()) {
        std::abort();
    }

    std::atomic<bool> waiterStarted{false};
    std::atomic<bool> markAcquired{false};
    std::thread waiter([&] {
        waiterStarted.store(true, std::memory_order_release);
        if (controller.AcquireMark()) {
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
    if (!markAcquired.load(std::memory_order_acquire) ||
        controller.GetOwner() != MSGCWorkerPoolOwner::IDLE) {
        std::abort();
    }
}

void TestEnumRootsWaitsForSweepHandoff()
{
    MSGCWorkerPoolController controller;
    if (!controller.TryAcquireSweep()) {
        std::abort();
    }

    std::atomic<bool> waiterStarted{false};
    std::atomic<bool> enumRootsAcquired{false};
    std::thread waiter([&] {
        waiterStarted.store(true, std::memory_order_release);
        if (controller.AcquireEnumRoots()) {
            enumRootsAcquired.store(true, std::memory_order_release);
            if (!controller.Release(MSGCWorkerPoolOwner::ENUM_ROOTS)) {
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
    if (enumRootsAcquired.load(std::memory_order_acquire)) {
        std::abort();
    }

    if (!controller.Release(MSGCWorkerPoolOwner::SWEEP)) {
        std::abort();
    }
    waiter.join();
    if (!enumRootsAcquired.load(std::memory_order_acquire) ||
        controller.GetOwner() != MSGCWorkerPoolOwner::IDLE) {
        std::abort();
    }
}

void TestStoppingRejectsNewOwners()
{
    MSGCWorkerPoolController controller;
    controller.BeginStop();
    if (controller.GetOwner() != MSGCWorkerPoolOwner::STOPPING ||
        controller.TryAcquireSweep() || controller.AcquireEnumRoots() || controller.AcquireMark()) {
        std::abort();
    }
}

} // namespace

int main()
{
    TestSweepIsNonBlockingAndExclusive();
    TestMarkWaitsForSweepHandoff();
    TestEnumRootsWaitsForSweepHandoff();
    TestStoppingRejectsNewOwners();
    return 0;
}
