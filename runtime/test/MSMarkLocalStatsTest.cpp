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

void TestWorkerProcessedCountRemainsLocalUntilExit()
{
    MSMarkWorkerLocalStats stats;
    stats.RecordScanned(1024);
    stats.RecordScanned(123);
    ExpectEq(stats.TotalProcessed(), 1147,
             "processed count should remain locally accumulated until worker exit");
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
    TestWorkerProcessedCountRemainsLocalUntilExit();
    TestAssistStatsMergeAndReset();
    return 0;
}
