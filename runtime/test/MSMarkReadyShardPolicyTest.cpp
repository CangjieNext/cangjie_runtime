#include <cstdio>
#include <cstdlib>
#include <vector>

#include "src/Heap/MCollector/MSMarkReadyShardPolicy.h"

namespace {

using MapleRuntime::ComputeMSMarkReadyShardCount;
using MapleRuntime::ResolveMSMarkReadyPreferredShard;
using MapleRuntime::TryMSMarkReadyAssistShardProbe;
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
    size_t localCursor = 5;
    bool claimed = TryMSMarkReadyAssistShardProbe(
        8, 3, localCursor,
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
    ExpectEq(localCursor, 7,
             "successful bounded stealing must advance only caller state");
}

void TestEachMissChecksOnlyTwoRemoteShardsAndAdvancesLocally()
{
    std::vector<size_t> firstAttempt;
    size_t localCursor = 0;
    bool claimed = TryMSMarkReadyAssistShardProbe(
        8, 0, localCursor,
        [&firstAttempt](size_t shard) {
            firstAttempt.push_back(shard);
            return false;
        });
    Expect(!claimed, "bounded local miss must return without a full scan");
    ExpectEq(firstAttempt.size(), 3,
             "a miss must probe home plus exactly two remote shards");
    ExpectEq(firstAttempt[0], 0, "first attempt must begin at home");
    ExpectEq(firstAttempt[1], 1, "home must be skipped by remote cursor");
    ExpectEq(firstAttempt[2], 2, "first attempt must stop after two remotes");
    ExpectEq(localCursor, 3, "miss must advance caller-local steal cursor");

    std::vector<size_t> secondAttempt;
    claimed = TryMSMarkReadyAssistShardProbe(
        8, 0, localCursor,
        [&secondAttempt](size_t shard) {
            secondAttempt.push_back(shard);
            return shard == 4;
        });
    Expect(claimed, "later local probes must eventually discover remote work");
    ExpectEq(secondAttempt.size(), 3,
             "later success must remain bounded to two remotes");
    ExpectEq(secondAttempt[1], 3, "local cursor must continue, not restart");
    ExpectEq(secondAttempt[2], 4, "second local window must find its target");
    ExpectEq(localCursor, 5, "successful second window must advance cursor");
}

void TestGCProbeRetainsCompleteFallback()
{
    std::vector<size_t> visited;
    bool claimed = TryMSMarkReadyShardProbe(
        8, 0, 1, true,
        [&visited](size_t shard) {
            visited.push_back(shard);
            return shard == 6;
        });
    Expect(claimed,
           "GC-owned lookup must find work beyond its bounded common path");
    Expect(visited.size() > 5,
           "GC-owned lookup must use the complete fallback when needed");
}

} // namespace

int main()
{
    TestShardCountUsesHalfOfAllParticipants();
    TestParticipantAffinityWrapsByShardCount();
    TestLimitedProbeStartsWithPreferredThenRotates();
    TestEachMissChecksOnlyTwoRemoteShardsAndAdvancesLocally();
    TestGCProbeRetainsCompleteFallback();
    return 0;
}
