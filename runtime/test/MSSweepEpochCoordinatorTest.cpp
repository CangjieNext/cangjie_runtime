#include <cstdio>
#include <cstdlib>

#include "src/Heap/MAllocator/MSSweepEpochCoordinator.h"

namespace {
using MapleRuntime::MSSweepEpochCoordinator;
void Expect(bool condition, const char* message) { if (!condition) { std::fprintf(stderr, "%s\n", message); std::abort(); } }
void TestEpochAdmissionLifecycle()
{
    MSSweepEpochCoordinator coordinator;
    Expect(coordinator.IsSweepDone(), "fresh coordinator must be done");
    coordinator.BeginEpoch(true);
    Expect(!coordinator.IsSweepDone() && coordinator.TryEnterSweep(), "pending epoch admits workers");
    coordinator.LeaveSweep();
    coordinator.CloseSweepAdmissionAndWait();
    Expect(!coordinator.TryEnterSweep(), "closed epoch rejects new workers");
    coordinator.PublishSweepDoneAfterQuiescence();
    Expect(coordinator.IsSweepDone(), "quiescent epoch publishes completion");
    coordinator.OpenSweepAdmission();
    Expect(coordinator.TryEnterSweep(), "reopened epoch admits workers");
    coordinator.LeaveSweep();
}
}
int main() { TestEpochAdmissionLifecycle(); return 0; }
