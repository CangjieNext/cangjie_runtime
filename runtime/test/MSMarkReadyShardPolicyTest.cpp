#include <cstdio>
#include <cstdlib>
#include <vector>

#include "src/Heap/MCollector/MSMarkReadyShardPolicy.h"

namespace {

using MapleRuntime::ComputeMSMarkReadyShardCount;
using MapleRuntime::ResolveMSMarkReadyPreferredShard;
using MapleRuntime::TryMSMarkReadyShardProbe;

void ExpectEq(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n",
                     message, expected, actual);
        std::abort();
    }
}

void Expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void TestShardCountUsesHalfOfAllParticipants()
{
    ExpectEq(ComputeMSMarkReadyShardCount(8, 8), 8,
             "8P plus 8 total GC threads should use 8 shards");
    ExpectEq(ComputeMSMarkReadyShardCount(2, 3), 3,
             "odd participant count should round half upward");
    ExpectEq(ComputeMSMarkReadyShardCount(0, 0), 1,
             "ready scheduler must retain one shard");
}

void TestParticipantAffinityWrapsByShardCount()
{
    ExpectEq(ResolveMSMarkReadyPreferredShard(0, 4), 0,
             "first P owns first shard");
    ExpectEq(ResolveMSMarkReadyPreferredShard(5, 4), 1,
             "GC/helper participant identity wraps by shard count");
    ExpectEq(ResolveMSMarkReadyPreferredShard(99, 0), 0,
             "unconfigured scheduler falls back to shard zero");
}

void TestLimitedProbeStartsWithPreferredThenRotates()
{
    std::vector<size_t> visited;
    bool claimed = TryMSMarkReadyShardProbe(
        8, 3, 5, false,
        [&visited](size_t shard) {
            visited.push_back(shard);
            return shard == 6;
        });
    Expect(claimed, "bounded rotating probe should find a nearby shard");
    ExpectEq(visited.size(), 3,
             "probe should stop immediately after a successful claim");
    ExpectEq(visited[0], 3, "preferred shard must be probed first");
    ExpectEq(visited[1], 5, "remote probing must start at rotation cursor");
    ExpectEq(visited[2], 6, "remote probing must advance round-robin");
}

void TestFullFallbackOnlyWhenLedgerIsNonzero()
{
    std::vector<size_t> withoutFallback;
    bool claimed = TryMSMarkReadyShardProbe(
        12, 0, 2, false,
        [&withoutFallback](size_t shard) {
            withoutFallback.push_back(shard);
            return shard == 10;
        });
    Expect(!claimed, "zero-ledger miss must stay bounded");
    Expect(withoutFallback.size() < 12,
           "zero-ledger miss must not scan every shard");

    std::vector<size_t> withFallback;
    claimed = TryMSMarkReadyShardProbe(
        12, 0, 2, true,
        [&withFallback](size_t shard) {
            withFallback.push_back(shard);
            return shard == 10;
        });
    Expect(claimed, "nonzero ledger must enable a complete fallback scan");
    Expect(withFallback.size() > withoutFallback.size(),
           "fallback must extend beyond the bounded probe set");
}

} // namespace

int main()
{
    TestShardCountUsesHalfOfAllParticipants();
    TestParticipantAffinityWrapsByShardCount();
    TestLimitedProbeStartsWithPreferredThenRotates();
    TestFullFallbackOnlyWhenLedgerIsNonzero();
    return 0;
}

