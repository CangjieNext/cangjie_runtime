#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "src/Heap/MCollector/MSMarkWorkerControl.h"
#include "src/Common/WorkEpoch.h"

namespace {

using MapleRuntime::MSMarkWorkerControl;
using MapleRuntime::WorkEpoch;

void ExpectTrue(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void ExpectEq(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n", message, expected, actual);
        std::abort();
    }
}

void TestQuiesceClosesClaimsAndWaitsForExistingWork()
{
    MSMarkWorkerControl control;
    control.Start(2);
    ExpectTrue(control.TryEnterWorker(0), "active helper should enter");
    ExpectTrue(control.TryEnterAssist(), "assist should enter while running");
    MSMarkWorkerControl::Snapshot active = control.GetSnapshot();
    ExpectEq(active.workersInFlight, 1, "one helper in flight");
    ExpectEq(active.assistsInFlight, 1, "one assist in flight");

    control.BeginQuiesce();
    ExpectTrue(!control.TryEnterWorker(1), "quiesce should reject new helper");
    ExpectTrue(!control.TryEnterAssist(), "quiesce should reject new assist");

    control.ExitWorker();
    control.ExitAssist();
    control.WaitForNoInFlight();
    MSMarkWorkerControl::Snapshot drained = control.GetSnapshot();
    ExpectEq(drained.workersInFlight, 0, "helpers should drain");
    ExpectEq(drained.assistsInFlight, 0, "assists should drain");
}

void TestQuiesceThenStopKeepsClaimsClosed()
{
    MSMarkWorkerControl control;
    control.Start(1);
    control.BeginQuiesce();
    control.Stop();
    ExpectTrue(!control.TryEnterWorker(0), "stopped epoch should reject helpers");
    ExpectTrue(!control.TryEnterAssist(), "stopped epoch should reject assists");
}

void TestSnapshotReturnsSchedulingAndInFlightState()
{
    MSMarkWorkerControl control;
    control.Start(2);
    ExpectTrue(control.TryEnterWorker(0), "worker should enter before snapshot");
    ExpectTrue(control.TryEnterAssist(), "assist should enter before snapshot");

    MSMarkWorkerControl::Snapshot snapshot = control.GetSnapshot();
    ExpectEq(snapshot.targetHelpers, 2, "snapshot should report helper target");
    ExpectEq(snapshot.workersInFlight, 1, "snapshot should report worker in flight");
    ExpectEq(snapshot.assistsInFlight, 1, "snapshot should report assist in flight");

    control.ExitWorker();
    control.ExitAssist();
    control.BeginQuiesce();
    control.WaitForNoInFlight();
    control.Stop();
}

void TestTargetGrowthWakesParkedWorker()
{
    MSMarkWorkerControl control;
    control.Start(1);
    std::atomic<bool> eligible{false};
    std::thread worker([&control, &eligible] {
        eligible.store(control.WaitUntilEligible(1), std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ExpectTrue(!eligible.load(std::memory_order_acquire),
               "worker outside the target should remain parked");

    control.SetTargetHelpers(2);
    worker.join();
    ExpectTrue(eligible.load(std::memory_order_acquire),
               "growing the target should wake the newly eligible worker");

    control.BeginQuiesce();
    control.WaitForNoInFlight();
    control.Stop();
}

void TestWaitForNoInFlightWaitsForBothKindsOfClaim()
{
    MSMarkWorkerControl control;
    control.Start(1);
    ExpectTrue(control.TryEnterWorker(0), "worker should enter");
    ExpectTrue(control.TryEnterAssist(), "assist should enter");
    control.BeginQuiesce();

    std::atomic<bool> waitFinished{false};
    std::thread waiter([&control, &waitFinished] {
        control.WaitForNoInFlight();
        waitFinished.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ExpectTrue(!waitFinished.load(std::memory_order_acquire),
               "wait must not finish while worker and assist are active");
    control.ExitWorker();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ExpectTrue(!waitFinished.load(std::memory_order_acquire),
               "wait must not finish while assist remains active");
    control.ExitAssist();
    waiter.join();
    ExpectTrue(waitFinished.load(std::memory_order_acquire),
               "wait should finish after both counters reach zero");
    control.Stop();
}

void TestLastClaimExitNotifiesProgressEpoch()
{
    WorkEpoch epoch;
    MSMarkWorkerControl control(&epoch);
    control.Start(1);
    ExpectTrue(control.TryEnterWorker(0), "worker should enter before progress test");
    uint64_t beforeWorkerExit = epoch.Load();
    control.ExitWorker();
    ExpectTrue(epoch.Load() > beforeWorkerExit,
               "last worker exit should wake GC main through the progress epoch");

    ExpectTrue(control.TryEnterAssist(), "assist should enter before progress test");
    uint64_t beforeAssistExit = epoch.Load();
    control.ExitAssist();
    ExpectTrue(epoch.Load() > beforeAssistExit,
               "last assist exit should wake GC main through the progress epoch");

    control.BeginQuiesce();
    control.WaitForNoInFlight();
    control.Stop();
}

void TestTargetAndPhaseChangesNotifyProgressEpoch()
{
    WorkEpoch epoch;
    MSMarkWorkerControl control(&epoch);
    uint64_t beforeStart = epoch.Load();
    control.Start(1);
    ExpectTrue(epoch.Load() > beforeStart, "starting an epoch should publish progress");

    uint64_t beforeTarget = epoch.Load();
    control.SetTargetHelpers(2);
    ExpectTrue(epoch.Load() > beforeTarget, "target growth should publish progress");

    uint64_t beforeQuiesce = epoch.Load();
    control.BeginQuiesce();
    ExpectTrue(epoch.Load() > beforeQuiesce, "quiesce should publish progress");
    control.WaitForNoInFlight();
    control.Stop();
}

void TestWorkerContinuationStaysInsideOneClaimUntilQuiesce()
{
    MSMarkWorkerControl control;
    control.Start(1);
    ExpectTrue(control.TryEnterWorker(0), "worker should enter one active burst");
    ExpectTrue(control.CanContinueWorker(0),
               "running eligible worker should continue the current burst");

    control.BeginQuiesce();
    ExpectTrue(!control.CanContinueWorker(0),
               "quiesce should stop continuation at the next WorkBuf boundary");
    control.ExitWorker();
    control.WaitForNoInFlight();
    control.Stop();
}

} // namespace

int main()
{
    TestQuiesceClosesClaimsAndWaitsForExistingWork();
    TestQuiesceThenStopKeepsClaimsClosed();
    TestSnapshotReturnsSchedulingAndInFlightState();
    TestTargetGrowthWakesParkedWorker();
    TestWaitForNoInFlightWaitsForBothKindsOfClaim();
    TestLastClaimExitNotifiesProgressEpoch();
    TestTargetAndPhaseChangesNotifyProgressEpoch();
    TestWorkerContinuationStaysInsideOneClaimUntilQuiesce();
    return 0;
}
