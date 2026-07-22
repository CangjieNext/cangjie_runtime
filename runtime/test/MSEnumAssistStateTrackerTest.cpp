#include <cstdio>
#include <cstdlib>

#include "src/Mutator/MSEnumAssistState.h"

namespace {

using MapleRuntime::MSEnumAssistStateTracker;

void ExpectTrue(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "%s: expected true\n", message);
        std::abort();
    }
}

void ExpectFalse(bool value, const char* message)
{
    if (value) {
        std::fprintf(stderr, "%s: expected false\n", message);
        std::abort();
    }
}

void TestEpochIsolationAndSingleCompletion()
{
    MSEnumAssistStateTracker tracker;

    tracker.Prepare(7);
    ExpectTrue(tracker.IsPending(7), "epoch 7 should be pending after prepare");
    ExpectFalse(tracker.IsPending(8), "different epoch should not observe pending");
    ExpectFalse(tracker.TryComplete(8), "stale epoch must not complete pending state");
    ExpectTrue(tracker.IsPending(7), "stale completion must not consume pending state");

    ExpectTrue(tracker.TryComplete(7), "matching epoch should complete pending state");
    ExpectFalse(tracker.TryComplete(7), "completion must only succeed once");
    ExpectFalse(tracker.IsPending(7), "epoch 7 should no longer be pending after completion");
}

void TestPrepareRearmsAndResetHonorsEpoch()
{
    MSEnumAssistStateTracker tracker;

    tracker.Prepare(11);
    ExpectTrue(tracker.TryComplete(11), "epoch 11 should complete once");

    tracker.Prepare(12);
    ExpectTrue(tracker.IsPending(12), "prepare should rearm tracker for new epoch");
    tracker.ResetIfEpochMatches(11);
    ExpectTrue(tracker.IsPending(12), "stale reset must not clear current epoch");

    tracker.ResetIfEpochMatches(12);
    ExpectFalse(tracker.IsPending(12), "matching reset should clear current epoch");
}

} // namespace

int main()
{
    TestEpochIsolationAndSingleCompletion();
    TestPrepareRearmsAndResetHonorsEpoch();
    return 0;
}
