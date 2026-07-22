#include <cstdio>
#include <cstdlib>

#include "src/Heap/Collector/AsyncTaskState.h"

namespace {

void ExpectTrue(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void TestSamePriorityCoalescesAcrossPendingAndRunning()
{
    MapleRuntime::AsyncTaskState state;
    constexpr uint32_t priority = 5;

    ExpectTrue(state.Push(priority, false), "first push should become pending");
    ExpectTrue(state.Pop() == priority, "pending task should move to running");

    ExpectTrue(!state.Push(priority, false), "running task should reject a duplicate push");
    ExpectTrue(state.Pop() == MapleRuntime::AsyncTaskState::kInvalidPriority,
               "running duplicate must not create pending work");

    state.Complete(priority);
    ExpectTrue(state.Push(priority, false), "completed task should be accepted again");
    ExpectTrue(state.Pop() == priority, "accepted task should be available after completion");
}

} // namespace

int main()
{
    TestSamePriorityCoalescesAcrossPendingAndRunning();
    return 0;
}
