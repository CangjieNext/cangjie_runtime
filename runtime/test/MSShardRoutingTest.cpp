#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define MRT_USE_MARK_SWEEP 1
#include "src/Heap/MAllocator/MSShardRouting.h"

namespace {

void ExpectEq(uint16_t actual, uint16_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n",
                     message,
                     static_cast<unsigned>(expected),
                     static_cast<unsigned>(actual));
        std::abort();
    }
}

void TestExplicitShardHintWins()
{
    MapleRuntime::MSShardRouteContext context {};
    context.shardCount = 8;
    context.hasBoundProcessor = true;
    context.processorId = 5;
    context.tid = 1234;

    ExpectEq(MapleRuntime::ResolveMSHomeShardId(3, context), 3, "explicit shard hint should win");
}

void TestProcessorBindingFallsBackToProcessorId()
{
    MapleRuntime::MSShardRouteContext context {};
    context.shardCount = 8;
    context.hasBoundProcessor = true;
    context.processorId = 13;
    context.tid = 1234;

    ExpectEq(MapleRuntime::ResolveMSHomeShardId(MapleRuntime::kMSInvalidShardId, context),
             5,
             "bound processor should map by processor id");
}

void TestUnboundThreadFallsBackToTidHash()
{
    MapleRuntime::MSShardRouteContext context {};
    context.shardCount = 8;
    context.hasBoundProcessor = false;
    context.processorId = 0;
    context.tid = 1235;

    ExpectEq(MapleRuntime::ResolveMSHomeShardId(MapleRuntime::kMSInvalidShardId, context),
             3,
             "unbound thread should map by tid hash");
}

void TestZeroShardCountDegeneratesToSingleShard()
{
    MapleRuntime::MSShardRouteContext context {};
    context.shardCount = 0;
    context.hasBoundProcessor = true;
    context.processorId = 99;
    context.tid = 77;

    ExpectEq(MapleRuntime::ResolveMSHomeShardId(MapleRuntime::kMSInvalidShardId, context),
             0,
             "zero shard count should normalize to shard 0");
}

void TestInitialCommittedHeapBytesScalesWithShardCount()
{
    constexpr size_t k64MB = 64ULL * 1024ULL * 1024ULL;
    if (MapleRuntime::ComputeMSInitialCommittedHeapBytes(0) != k64MB) {
        std::fprintf(stderr, "initial committed heap for shardCount=0 should be 64MB\n");
        std::abort();
    }
    if (MapleRuntime::ComputeMSInitialCommittedHeapBytes(4) != 4 * k64MB) {
        std::fprintf(stderr, "initial committed heap for shardCount=4 should be 256MB\n");
        std::abort();
    }
}

} // namespace

int main()
{
    TestExplicitShardHintWins();
    TestProcessorBindingFallsBackToProcessorId();
    TestUnboundThreadFallsBackToTidHash();
    TestZeroShardCountDegeneratesToSingleShard();
    TestInitialCommittedHeapBytesScalesWithShardCount();
    return 0;
}
