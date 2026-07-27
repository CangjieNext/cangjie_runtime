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

void ExpectEq32(uint32_t actual, uint32_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n", message, expected, actual);
        std::abort();
    }
}

void ExpectEqSize(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n", message, expected, actual);
        std::abort();
    }
}

void ExpectTrue(bool actual, const char* message)
{
    if (!actual) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void ExpectFalse(bool actual, const char* message)
{
    if (actual) {
        std::fprintf(stderr, "%s\n", message);
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

void TestAutomaticShardPolicy()
{
    struct Case {
        uint32_t processorCount;
        uint32_t expectedShardCount;
    };
    constexpr Case cases[] = {
        {1, 1},
        {8, 8},
        {16, 8},
        {32, 8},
        {64, 8},
        {128, 16},
        {1024, 32},
        {127, 1},
    };

    for (const Case& item : cases) {
        uint32_t actual = MapleRuntime::ResolveMSShardCount(item.processorCount, 0);
        ExpectEq32(actual, item.expectedShardCount, "automatic shard policy mismatch");
        ExpectEq32(item.processorCount % actual, 0, "automatic shard count must divide processor count");
    }
}

void TestConfiguredShardPolicyNormalizesToProcessorDivisor()
{
    ExpectEq32(MapleRuntime::ResolveMSShardCount(32, 24), 16,
               "configured shard count should round down to a processor divisor");
    ExpectEq32(MapleRuntime::ResolveMSShardCount(32, 128), 32,
               "configured shard count should clamp to processor count");
    ExpectEq32(MapleRuntime::ResolveMSShardCount(16, 7), 4,
               "configured shard count should select the largest lower divisor");
    ExpectEq32(MapleRuntime::ResolveMSShardCount(0, 0), 1,
               "zero processor count should normalize to one processor and shard");
}

void TestMaximumShardCountKeepsInvalidIdReserved()
{
    uint32_t shardCount = MapleRuntime::ResolveMSShardCount(
        MapleRuntime::kMSMaxShardCount, MapleRuntime::kMSMaxShardCount);
    ExpectEq32(shardCount, MapleRuntime::kMSMaxShardCount,
               "maximum representable shard count should remain available");

    MapleRuntime::MSShardRouteContext context {};
    context.shardCount = shardCount;
    context.hasBoundProcessor = true;
    context.processorId = shardCount - 1;
    ExpectEq(MapleRuntime::ResolveMSHomeShardId(MapleRuntime::kMSInvalidShardId, context),
             static_cast<uint16_t>(shardCount - 1),
             "the highest valid shard id should remain below the invalid sentinel");
}

void TestInitialSegmentPolicyIsIndependentFromShardCount()
{
    constexpr size_t k64MB = 64ULL * 1024ULL * 1024ULL;
    constexpr size_t k16GBKB = 16ULL * 1024ULL * 1024ULL;
    constexpr size_t k64GBKB = 64ULL * 1024ULL * 1024ULL;
    ExpectEqSize(MapleRuntime::ResolveMSInitialSegmentCount(0), 1,
                 "automatic initial segment count should default to one");
    ExpectEqSize(MapleRuntime::ResolveMSInitialSegmentCount(4), 4,
                 "configured initial segment count should be preserved");
    ExpectEqSize(MapleRuntime::ResolveMSInitialSegmentCount(1024), 1024,
                 "rollback initial segment count must not be clamped by the default virtual heap");
    size_t resolvedVirtualHeapSizeKB = 0;
    ExpectTrue(MapleRuntime::TryResolveMSRequiredVirtualHeapSizeKB(
                   1, k16GBKB, resolvedVirtualHeapSizeKB),
               "default initial segment and virtual heap should be representable");
    ExpectEqSize(resolvedVirtualHeapSizeKB, k16GBKB,
                 "default initial segment should preserve the configured virtual heap");
    ExpectTrue(MapleRuntime::TryResolveMSRequiredVirtualHeapSizeKB(
                   1024, k16GBKB, resolvedVirtualHeapSizeKB),
               "rollback initial segments and virtual heap should be representable");
    ExpectEqSize(resolvedVirtualHeapSizeKB, k64GBKB,
                 "rollback initial segments should expand the effective virtual heap to 64GB");

    size_t committedHeapBytes = 0;
    ExpectTrue(MapleRuntime::TryComputeMSInitialCommittedHeapBytes(1, committedHeapBytes),
               "one initial segment should be representable");
    ExpectEqSize(committedHeapBytes, k64MB,
                 "one initial segment should commit 64MB");
    ExpectTrue(MapleRuntime::TryComputeMSInitialCommittedHeapBytes(4, committedHeapBytes),
               "four initial segments should be representable");
    ExpectEqSize(committedHeapBytes, 4 * k64MB,
                 "four initial segments should commit 256MB");
    ExpectFalse(MapleRuntime::TryComputeMSInitialCommittedHeapBytes(
                    MapleRuntime::kMSMaxInitialSegmentCount + 1, committedHeapBytes),
                "initial Segment count above the byte-safe limit must be rejected");
    ExpectTrue(MapleRuntime::kMSMaxVirtualHeapSizeBytes <=
                   std::numeric_limits<size_t>::max() / 2,
               "maximum virtual heap must leave address space for metadata and alignment");
    ExpectTrue(MapleRuntime::kMSMaxInitialSegmentCount * MapleRuntime::kMSRegionsPerShardSegment <=
                   std::numeric_limits<uint32_t>::max(),
               "maximum virtual heap must remain representable by RegionIndex");
    ExpectTrue(MapleRuntime::kMSMaxInitialSegmentCount * MapleRuntime::kMSUnitsPerShardSegment <=
                   std::numeric_limits<uint32_t>::max(),
               "maximum virtual heap must remain representable by UnitIndex on 4KB pages");
    ExpectFalse(MapleRuntime::TryResolveMSRequiredVirtualHeapSizeKB(
                    1, MapleRuntime::kMSMaxVirtualHeapSizeKB + 1, resolvedVirtualHeapSizeKB),
                "max virtual heap above the complete-layout-safe limit must be rejected");
}

} // namespace

int main()
{
    TestExplicitShardHintWins();
    TestProcessorBindingFallsBackToProcessorId();
    TestUnboundThreadFallsBackToTidHash();
    TestZeroShardCountDegeneratesToSingleShard();
    TestAutomaticShardPolicy();
    TestConfiguredShardPolicyNormalizesToProcessorDivisor();
    TestMaximumShardCountKeepsInvalidIdReserved();
    TestInitialSegmentPolicyIsIndependentFromShardCount();
    return 0;
}
