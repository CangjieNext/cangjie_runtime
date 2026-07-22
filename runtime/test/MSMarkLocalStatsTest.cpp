#include <cstdio>
#include <cstdlib>

#include "src/Heap/MCollector/MSMarkLocalStats.h"

namespace {

using MapleRuntime::MSMarkAssistStats;
using MapleRuntime::MSMarkWorkerLocalStats;

void ExpectEq(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n", message, expected, actual);
        std::abort();
    }
}

void TestWorkerCreditPublishesInBatches()
{
    MSMarkWorkerLocalStats stats(4);
    stats.RecordScanned(1024);
    stats.RecordScanned(1024);
    stats.RecordScanned(1024);
    ExpectEq(stats.TakePublishedCredit(), 0, "three buffers should remain worker-local");

    stats.RecordScanned(1024);
    ExpectEq(stats.TakePublishedCredit(), 4096, "four buffers should publish one credit batch");
    ExpectEq(stats.TotalProcessed(), 4096, "processed count should remain locally accumulated");
}

void TestWorkerCreditFlushesTail()
{
    MSMarkWorkerLocalStats stats(4);
    stats.RecordScanned(123);
    stats.PublishPendingCredit();
    ExpectEq(stats.TakePublishedCredit(), 123, "idle/quiesce flush should publish the tail");
    ExpectEq(stats.TakePublishedCredit(), 0, "published credit should be consumed once");
}

void TestAssistStatsMergeAndReset()
{
    MSMarkAssistStats owner;
    owner.processed = 900;

    MSMarkAssistStats total;
    total.Merge(owner);
    ExpectEq(total.processed, 900, "actual scanned objects should merge");

    MSMarkAssistStats snapshot = owner.TakeAndReset();
    ExpectEq(snapshot.processed, 900, "take should preserve the snapshot");
    ExpectEq(owner.processed, 0, "take should reset owner processed count");
}

} // namespace

int main()
{
    TestWorkerCreditPublishesInBatches();
    TestWorkerCreditFlushesTail();
    TestAssistStatsMergeAndReset();
    return 0;
}
