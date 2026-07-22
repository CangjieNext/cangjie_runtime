#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "src/Common/LFStack.h"
#include "src/Common/WorkEpoch.h"

namespace {

using MapleRuntime::LFStack;
using MapleRuntime::BaseObject;
using MapleRuntime::PerPGCWork;
using MapleRuntime::WorkBuf;
using MapleRuntime::WorkBufPool;
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

void TestFullCountTracksAllRemovalPaths()
{
    LFStack stack;
    WorkBuf partial;
    WorkBuf full;
    partial.len = 1;
    full.len = WorkBuf::kCapacity;

    stack.Push(&partial);
    stack.Push(&full);
    LFStack::Counts counts = stack.GetCounts();
    ExpectEq(counts.total, 2, "two published buffers after push");
    ExpectEq(counts.full, 1, "one full buffer after push");

    ExpectTrue(stack.PopFull() == &full, "PopFull should return full buffer");
    ExpectEq(stack.GetCounts().full, 0, "no full buffers after PopFull");

    full.len = WorkBuf::kCapacity;
    stack.Push(&full);
    ExpectEq(stack.GetCounts().full, 1, "full count should recover after repush");
    ExpectTrue(stack.Pop() == &full, "Pop should return top full buffer");
    ExpectEq(stack.GetCounts().full, 0, "Pop should decrement full count");
    ExpectTrue(stack.Pop() == &partial, "partial buffer should remain");
}

void TestGCConsumerPrioritizesFullLane()
{
    LFStack stack;
    WorkBuf full;
    WorkBuf partial;
    full.len = WorkBuf::kCapacity;
    partial.len = 1;

    stack.Push(&full);
    stack.Push(&partial);

    ExpectTrue(stack.Pop() == &full,
               "GC consumer should prefer ready full work over a newer partial tail");
    ExpectEq(stack.GetCounts().full, 0, "full lane should be empty after preferred pop");
    ExpectTrue(stack.Pop() == &partial, "partial tail should remain available after full work");
    ExpectTrue(stack.Empty(), "both lanes should be empty after both buffers are consumed");
}

void TestTrustedGreyValidationMatchesBuildMode()
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    ExpectTrue(WorkBuf::kRevalidateTrustedGrey,
               "debug builds must revalidate trusted-grey entries");
#else
    ExpectTrue(!WorkBuf::kRevalidateTrustedGrey,
               "release builds must not revalidate trusted-grey entries");
#endif
}

void TestGCClaimAcceptsPartialBuffer()
{
    LFStack global;
    WorkBufPool pool;
    PerPGCWork work;
    work.Init(&global, &pool);

    WorkBuf* partial = pool.Get();
    partial->Push(reinterpret_cast<BaseObject*>(1));
    global.Push(partial);

    ExpectTrue(work.AcquireOneBuffer(), "GC claim should accept a published partial buffer");
    ExpectTrue(work.getCur_ == partial, "GC claim should install the partial buffer as getCur");
    ExpectEq(global.GetCounts().total, 0, "claimed partial buffer should leave Global");
    work.Reset();
}

void TestAssistClaimLeavesPartialBuffer()
{
    LFStack global;
    WorkBufPool pool;
    PerPGCWork work;
    work.Init(&global, &pool);

    WorkBuf* partial = pool.Get();
    partial->Push(reinterpret_cast<BaseObject*>(1));
    global.Push(partial);

    ExpectTrue(!work.AcquireOneFullBuffer(), "assist claim must remain full-only");
    ExpectEq(global.GetCounts().total, 1, "assist claim must leave partial work for GC workers");
    pool.Put(global.Pop());
    work.Reset();
}

void TestPrivatePartialWaitsForTermination()
{
    LFStack global;
    WorkBufPool pool;
    PerPGCWork work;
    work.Init(&global, &pool);
    work.PutObj(reinterpret_cast<BaseObject*>(1));

    ExpectTrue(global.Empty(), "partial output should remain private before flush");
    ExpectTrue(work.ConcurrentInputEmpty(), "private partial output must not block concurrent convergence");
    work.Dispose();
    ExpectEq(global.GetCounts().total, 1, "termination dispose should publish the private partial output");

    WorkBuf* published = global.Pop();
    ExpectEq(published->len, 1, "termination output should preserve its length");
    pool.Put(published);
    work.Reset();
}

void TestBatchPushAndPopPreserveLanesAndCounts()
{
    LFStack stack;
    WorkBuf full1;
    WorkBuf full2;
    WorkBuf partial;
    full1.len = WorkBuf::kCapacity;
    full2.len = WorkBuf::kCapacity;
    partial.len = 1;
    full1.next = &partial;
    partial.next = &full2;
    stack.PushChain(&full1);

    ExpectEq(stack.GetCounts().total, 3, "batch push should count every published buffer");
    ExpectEq(stack.GetCounts().full, 2, "batch push should classify every full buffer");

    WorkBuf* popped[3] = { nullptr, nullptr, nullptr };
    size_t got = stack.PopN(popped, 3);

    ExpectEq(got, 3, "batch pop should return every available buffer");
    ExpectTrue(popped[0] == &full1 && popped[1] == &full2,
               "batch pop should drain the full lane before partial tails");
    ExpectTrue(popped[2] == &partial, "batch pop should include partial work after full work");
    ExpectEq(stack.GetCounts().total, 0, "batch pop should update total count exactly");
    ExpectEq(stack.GetCounts().full, 0, "batch pop should update full count exactly");
}

void TestWorkBufPoolBatchReturnAndAcquire()
{
    WorkBufPool pool;
    WorkBuf* first = new WorkBuf();
    WorkBuf* second = new WorkBuf();
    WorkBuf* third = new WorkBuf();
    first->len = 1;
    second->len = WorkBuf::kCapacity;
    third->len = 2;
    first->next = second;
    second->next = third;

    pool.PutChain(first);

    WorkBuf* acquired[3] = { nullptr, nullptr, nullptr };
    size_t got = pool.GetN(acquired, 3);
    ExpectEq(got, 3, "batch pool acquire should return the whole returned chain");
    ExpectTrue(acquired[0] != acquired[1] && acquired[0] != acquired[2] && acquired[1] != acquired[2],
               "batch pool acquire should return every buffer exactly once");
    for (size_t i = 0; i < got; ++i) {
        ExpectTrue(acquired[i] != nullptr, "batch pool acquire must not return null entries");
        ExpectTrue(acquired[i]->Empty(), "batch-returned buffers must be reset before reuse");
        ExpectTrue(acquired[i]->next == nullptr, "acquired buffers must be detached from the free chain");
        pool.Put(acquired[i]);
    }
}

void TestGlobalPublicationAdvancesWorkEpoch()
{
    WorkEpoch epoch;
    LFStack global(&epoch);
    WorkBuf published;
    uint64_t before = epoch.Load();

    global.Push(&published);

    ExpectTrue(epoch.Load() > before, "publishing a WorkBuf should advance the work epoch");
    ExpectTrue(global.Pop() == &published, "published WorkBuf should remain consumable");
}

void TestGlobalPublicationWakesEpochWaiter()
{
    WorkEpoch epoch;
    LFStack global(&epoch);
    WorkBuf published;
    uint64_t observed = epoch.Load();
    std::atomic<bool> changed{false};
    std::thread waiter([&epoch, &changed, observed] {
        changed.store(epoch.WaitForChange(observed, std::chrono::milliseconds(100)),
                      std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    global.Push(&published);
    waiter.join();

    ExpectTrue(changed.load(std::memory_order_acquire),
               "publishing a WorkBuf should wake a GC-main epoch waiter");
    ExpectTrue(global.Pop() == &published, "wakeup must not consume the published WorkBuf");
}

void TestBatchPublicationAdvancesEpochOnce()
{
    WorkEpoch epoch;
    LFStack global(&epoch);
    WorkBuf first;
    WorkBuf second;
    first.next = &second;
    uint64_t before = epoch.Load();

    global.PushChain(&first);

    ExpectEq(static_cast<size_t>(epoch.Load() - before), 1,
             "one batch publication should produce one progress notification");
    WorkBuf* popped[2] = { nullptr, nullptr };
    ExpectEq(global.PopN(popped, 2), 2, "batch-published buffers should remain consumable");
}

void TestDisposeTailAdvancesWorkEpoch()
{
    WorkEpoch epoch;
    LFStack global(&epoch);
    WorkBufPool pool;
    PerPGCWork work;
    work.Init(&global, &pool);
    work.PutObj(reinterpret_cast<BaseObject*>(1));
    uint64_t before = epoch.Load();

    work.Dispose();

    ExpectTrue(epoch.Load() > before, "disposing a private tail should advance the work epoch");
    WorkBuf* published = global.Pop();
    ExpectTrue(published != nullptr, "dispose should publish the private tail");
    pool.Put(published);
    work.Reset();
}

} // namespace

int main()
{
    TestFullCountTracksAllRemovalPaths();
    TestGCConsumerPrioritizesFullLane();
    TestTrustedGreyValidationMatchesBuildMode();
    TestGCClaimAcceptsPartialBuffer();
    TestAssistClaimLeavesPartialBuffer();
    TestPrivatePartialWaitsForTermination();
    TestBatchPushAndPopPreserveLanesAndCounts();
    TestWorkBufPoolBatchReturnAndAcquire();
    TestGlobalPublicationAdvancesWorkEpoch();
    TestGlobalPublicationWakesEpochWaiter();
    TestBatchPublicationAdvancesEpochOnce();
    TestDisposeTailAdvancesWorkEpoch();
    return 0;
}
