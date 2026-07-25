#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <type_traits>

#include "src/Common/LFStack.h"
#include "src/Common/WorkEpoch.h"

namespace {

using MapleRuntime::LFStack;
using MapleRuntime::BaseObject;
using MapleRuntime::PerPGCWork;
using MapleRuntime::WorkBuf;
using MapleRuntime::WorkBufPool;
using MapleRuntime::WorkEpoch;

static_assert(std::is_standard_layout<PerPGCWork>::value,
              "PerPGCWork must remain standard-layout inside compiler-visible AllocBufferMS");

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

void TestPopWithCountsReturnsPostPopSnapshot()
{
    LFStack stack;
    WorkBuf full1;
    WorkBuf full2;
    WorkBuf partial;
    full1.len = WorkBuf::kCapacity;
    full2.len = WorkBuf::kCapacity;
    partial.len = 7;

    stack.Push(&partial);
    stack.Push(&full1);
    stack.Push(&full2);

    LFStack::PopResult result = stack.PopWithCounts();
    ExpectTrue(result.buf == &full2, "PopWithCounts should preserve Full-first LIFO order");
    ExpectEq(result.counts.total, 2, "PopWithCounts should return the post-pop total");
    ExpectEq(result.counts.full, 1, "PopWithCounts should return the post-pop Full count");

    result = stack.PopWithCounts();
    ExpectTrue(result.buf == &full1, "PopWithCounts should drain the remaining Full buffer");
    ExpectEq(result.counts.total, 1, "one Partial buffer should remain after both Full buffers");
    ExpectEq(result.counts.full, 0, "no Full buffers should remain");

    result = stack.PopWithCounts();
    ExpectTrue(result.buf == &partial, "PopWithCounts should fall back to Partial");
    ExpectEq(result.counts.total, 0, "PopWithCounts should report an empty queue after the final pop");
    ExpectEq(result.counts.full, 0, "empty queue should have no Full buffers");

    result = stack.PopWithCounts();
    ExpectTrue(result.buf == nullptr, "PopWithCounts should return null for an empty queue");
    ExpectEq(result.counts.total, 0, "empty PopWithCounts should retain an exact zero snapshot");
}

void TestApproxCountsTrackPublishedAndConsumedWork()
{
    WorkEpoch epoch;
    LFStack stack(&epoch);
    WorkBuf full;
    WorkBuf partial;
    full.len = WorkBuf::kCapacity;
    partial.len = 3;

    stack.Push(&partial);
    stack.Push(&full);
    LFStack::Counts counts = stack.GetApproxCounts();
    ExpectEq(counts.total, 2, "approximate snapshot should observe both published buffers");
    ExpectEq(counts.full, 1, "approximate snapshot should observe the Full lane");

    LFStack::PopResult result = stack.PopWithCounts();
    counts = stack.GetApproxCounts();
    ExpectEq(counts.total, result.counts.total,
             "approximate total should follow the exact post-pop snapshot");
    ExpectEq(counts.full, result.counts.full,
             "approximate Full count should follow the exact post-pop snapshot");
}

void TestFullAndPartialLanesCanBeClaimedIndependently()
{
    LFStack stack;
    WorkBuf full;
    WorkBuf partial;
    full.len = WorkBuf::kCapacity;
    partial.len = 17;

    stack.Push(&partial);
    stack.Push(&full);

    ExpectTrue(stack.PopPartial() == &partial,
               "Partial claim should not consume ready Full work");
    ExpectEq(stack.GetCounts().full, 1,
             "Full count should remain after independent Partial claim");
    ExpectTrue(stack.PopFull() == &full,
               "Full claim should consume the dedicated Full lane");
    ExpectTrue(stack.Empty(), "independent lane claims should drain both lanes");
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

void TestPerPGCWorkReusesDrainedInputWithoutGlobalPoolRoundTrip()
{
    LFStack global;
    WorkBufPool pool;
    PerPGCWork work;
    work.Init(&global, &pool);

    WorkBuf* input = pool.Get();
    input->Push(reinterpret_cast<BaseObject*>(1));
    global.Push(input);

    ExpectTrue(work.AcquireOneBuffer(), "GC work should claim the published input");
    ExpectTrue(work.getCur_->Pop() == reinterpret_cast<BaseObject*>(1),
               "test should drain the claimed input");
    work.RecycleCurrentInput();
    ExpectTrue(work.getCur_ == nullptr, "recycled input should leave the active get slot");

    work.PutObj(reinterpret_cast<BaseObject*>(2));
    ExpectTrue(work.putCur_ == input,
               "the next local output should reuse the drained input without touching the global pool");
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

void TestLocalPutBufferPromotionHonorsThreshold()
{
    LFStack global;
    WorkBufPool pool;
    PerPGCWork work;
    work.Init(&global, &pool);

    for (size_t i = 0; i < WorkBuf::kCapacity / 8; ++i) {
        work.PutObj(reinterpret_cast<BaseObject*>(i + 1));
    }
    WorkBuf* local = work.putCur_;

    ExpectTrue(work.PromotePutToGet(WorkBuf::kCapacity / 8),
               "128-object local output should promote to the next input");
    ExpectTrue(work.getCur_ == local,
               "promotion should transfer the exact local WorkBuf");
    ExpectTrue(work.putCur_ == nullptr,
               "promotion should leave output allocation lazy");
    ExpectTrue(global.Empty(),
               "local promotion must not publish through Global");

    work.Reset();
}

void TestLocalPutBufferPromotionCanDeferSmallTail()
{
    LFStack global;
    WorkBufPool pool;
    PerPGCWork work;
    work.Init(&global, &pool);
    work.PutObj(reinterpret_cast<BaseObject*>(1));

    ExpectTrue(!work.PromotePutToGet(WorkBuf::kCapacity / 8),
               "small local output should defer while Global work is preferred");
    ExpectTrue(work.PromotePutToGet(1),
               "small local output should promote after Global lanes are empty");

    work.Reset();
}

void TestAssistReusesAndPublishesOnePartialOutputBuffer()
{
    LFStack global;
    WorkBufPool pool;
    PerPGCWork work;
    work.Init(&global, &pool);

    WorkBuf* partial = pool.Get();
    partial->Push(reinterpret_cast<BaseObject*>(1));
    global.Push(partial);

    ExpectTrue(work.AcquirePartialOutputBuffer() == partial,
               "Assist output should reuse one published Partial buffer");
    ExpectTrue(global.Empty(),
               "claimed Partial output should leave Global");
    work.PutObj(reinterpret_cast<BaseObject*>(2));
    work.PublishPartialOutput();

    ExpectTrue(work.putCur_ == nullptr,
               "published Assist output must leave TLS ownership");
    WorkBuf* republished = global.PopPartial();
    ExpectTrue(republished == partial,
               "Assist output should republish the same Partial buffer");
    ExpectEq(republished->len, 2,
             "Assist output should preserve old entries and append new entries");
    pool.Put(republished);
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
    TestPopWithCountsReturnsPostPopSnapshot();
    TestApproxCountsTrackPublishedAndConsumedWork();
    TestFullAndPartialLanesCanBeClaimedIndependently();
    TestGCClaimAcceptsPartialBuffer();
    TestPerPGCWorkReusesDrainedInputWithoutGlobalPoolRoundTrip();
    TestAssistClaimLeavesPartialBuffer();
    TestPrivatePartialWaitsForTermination();
    TestLocalPutBufferPromotionHonorsThreshold();
    TestLocalPutBufferPromotionCanDeferSmallTail();
    TestAssistReusesAndPublishesOnePartialOutputBuffer();
    TestBatchPushAndPopPreserveLanesAndCounts();
    TestWorkBufPoolBatchReturnAndAcquire();
    TestGlobalPublicationAdvancesWorkEpoch();
    TestGlobalPublicationWakesEpochWaiter();
    TestBatchPublicationAdvancesEpochOnce();
    TestDisposeTailAdvancesWorkEpoch();
    return 0;
}
