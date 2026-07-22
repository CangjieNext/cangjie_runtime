#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define MRT_USE_MARK_SWEEP 1
#include "src/Heap/MAllocator/MSOOMEmergencyPolicy.h"

namespace {

using MapleRuntime::BuildMSPostMarkSweepPlan;
using MapleRuntime::TryMSAllocationWithEmergencyRetry;

void Expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void TestNormalGCUsesConcurrentSweepHandoff()
{
    auto plan = BuildMSPostMarkSweepPlan(false);
    Expect(plan.reenableAtMarkTermination, "normal GC must reopen sweep at MarkTermination");
    Expect(!plan.forceFinishInPostMark, "normal GC must not force-finish sweep in PostMark");
    Expect(!plan.flushAllCaches, "normal GC must retain normal cache policy");
    Expect(plan.notifyBackgroundSweep, "normal GC must notify background sweep");
}

void TestOOMGCUsesSynchronousForceSweep()
{
    auto plan = BuildMSPostMarkSweepPlan(true);
    Expect(!plan.reenableAtMarkTermination, "OOM GC must keep sweep admission closed");
    Expect(plan.forceFinishInPostMark, "OOM GC must force-finish sweep in PostMark");
    Expect(plan.flushAllCaches, "OOM GC must flush all reclaimable caches");
    Expect(!plan.notifyBackgroundSweep, "OOM GC must not schedule redundant background sweep");
}

void TestAllocationSuccessDoesNotRunEmergencyRecovery()
{
    size_t attempts = 0;
    size_t recoveries = 0;
    uintptr_t result = TryMSAllocationWithEmergencyRetry(
        true,
        [&attempts]() -> uintptr_t {
            ++attempts;
            return 0x1000;
        },
        [&recoveries]() { ++recoveries; });

    Expect(result == 0x1000, "successful first attempt must be returned");
    Expect(attempts == 1, "successful allocation must run once");
    Expect(recoveries == 0, "successful allocation must not request OOM recovery");
}

void TestAllocationRunsOneRecoveryAndOneRetry()
{
    size_t attempts = 0;
    size_t recoveries = 0;
    uintptr_t result = TryMSAllocationWithEmergencyRetry(
        true,
        [&attempts]() -> uintptr_t {
            ++attempts;
            return attempts == 2 ? 0x2000 : 0;
        },
        [&recoveries]() { ++recoveries; });

    Expect(result == 0x2000, "allocation must return the emergency retry result");
    Expect(attempts == 2, "allocation must perform exactly one retry");
    Expect(recoveries == 1, "allocation must perform exactly one emergency recovery");
}

void TestFailedRetryDoesNotLoop()
{
    size_t attempts = 0;
    size_t recoveries = 0;
    uintptr_t result = TryMSAllocationWithEmergencyRetry(
        true,
        [&attempts]() -> uintptr_t {
            ++attempts;
            return 0;
        },
        [&recoveries]() { ++recoveries; });

    Expect(result == 0, "failed emergency retry must return zero");
    Expect(attempts == 2, "failed emergency retry must not loop");
    Expect(recoveries == 1, "failed emergency retry must not request another GC");
}

void TestGCThreadFailureDoesNotRunEmergencyRecovery()
{
    size_t attempts = 0;
    size_t recoveries = 0;
    uintptr_t result = TryMSAllocationWithEmergencyRetry(
        false,
        [&attempts]() -> uintptr_t {
            ++attempts;
            return 0;
        },
        [&recoveries]() { ++recoveries; });

    Expect(result == 0, "GC-thread allocation failure must return zero");
    Expect(attempts == 1, "GC-thread allocation must not retry");
    Expect(recoveries == 0, "GC-thread allocation must not request recursive GC");
}

} // namespace

int main()
{
    TestNormalGCUsesConcurrentSweepHandoff();
    TestOOMGCUsesSynchronousForceSweep();
    TestAllocationSuccessDoesNotRunEmergencyRecovery();
    TestAllocationRunsOneRecoveryAndOneRetry();
    TestFailedRetryDoesNotLoop();
    TestGCThreadFailureDoesNotRunEmergencyRecovery();
    std::puts("MS OOM emergency policy tests passed");
    return 0;
}
