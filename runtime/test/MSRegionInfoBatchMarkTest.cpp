#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

#define MRT_USE_MARK_SWEEP 1
#define MRT_BASE_LOG_H

#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#define DCHECK(...)
#define LOG(...)
#define DLOG(...)
#define CHECK_DETAIL(x, ...)                                                                     \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::abort();                                                                        \
        }                                                                                        \
    } while (false)
#define MRT_ASSERT(x, msg)                                                                       \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::abort();                                                                        \
        }                                                                                        \
    } while (false)

namespace MapleRuntime {
const size_t MRT_PAGE_SIZE = 16 * 1024;
}

#include "src/Heap/MAllocator/MSBitmapArena.h"
#include "src/Heap/MAllocator/MSSizeClass.h"
#include "src/Heap/MSGCFastPath.h"

namespace MapleRuntime {

void MSBitmapArena::EnsurePlaneRangeCommitted(MSBitmapPlane, size_t, size_t)
{
}

uint64_t* MSBitmapArena::GetAllocPlane(size_t)
{
    return reinterpret_cast<uint64_t*>(CJ_MRT_MSGCFastPathState.allocPlaneBase);
}

uint64_t* MSBitmapArena::GetMarkPlane(size_t)
{
    return reinterpret_cast<uint64_t*>(CJ_MRT_MSGCFastPathState.markPlaneBase);
}

uint64_t* MSBitmapArena::GetScanPlane(size_t)
{
    return reinterpret_cast<uint64_t*>(CJ_MRT_MSGCFastPathState.scanPlaneBase);
}
} // namespace MapleRuntime

#include "src/Heap/MAllocator/MSRegionInfo.cpp"

namespace MapleRuntime {
extern "C" MSGCFastPathState CJ_MRT_MSGCFastPathState = {};
}

namespace {

using MapleRuntime::MSRegionInfo;
using MapleRuntime::MSRegionInfoArray;
using MapleRuntime::MSRegionKind;
using MapleRuntime::MSScanState;

struct RegionPlaneFixture {
    alignas(64) MSRegionInfo region {};
    alignas(64) uint64_t allocPlane[MapleRuntime::MSBitmapArena::kMaxBitmapWords] {};
    alignas(64) uint64_t markPlane[MapleRuntime::MSBitmapArena::kMaxBitmapWords] {};
    alignas(64) uint64_t scanPlane[MapleRuntime::MSBitmapArena::kMaxBitmapWords] {};

    RegionPlaneFixture()
    {
        region.sizeClass = 0; // 4096 slots / 64 bitmap words.
        region.SetRegionKind(MSRegionKind::SMALL);
        MapleRuntime::PublishMSGCFastPathRegionInfoArray(
            reinterpret_cast<uintptr_t>(&region));
        MapleRuntime::PublishMSGCFastPathPlaneBases(
            reinterpret_cast<uintptr_t>(allocPlane),
            reinterpret_cast<uintptr_t>(markPlane),
            reinterpret_cast<uintptr_t>(scanPlane));
    }

    ~RegionPlaneFixture()
    {
        MapleRuntime::PublishMSGCFastPathPlaneBases(0, 0, 0);
        MapleRuntime::PublishMSGCFastPathRegionInfoArray(0);
    }
};

void ExpectEq(uint64_t actual, uint64_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=0x%llx actual=0x%llx\n",
                     message,
                     static_cast<unsigned long long>(expected),
                     static_cast<unsigned long long>(actual));
        std::abort();
    }
}

void TestBatchMarkReturnsOnlyNewBits()
{
    RegionPlaneFixture fixture;
    fixture.markPlane[0] = 1ULL << 3;

    uint64_t mask = (1ULL << 3) | (1ULL << 7) | (1ULL << 11);
    uint64_t newBits = fixture.region.MarkBitmapWord(0, mask);

    ExpectEq(newBits, (1ULL << 7) | (1ULL << 11), "batch mark winners");
    ExpectEq(fixture.markPlane[0], mask, "batch mark bitmap");
    ExpectEq(fixture.region.MarkBitmapWord(0, mask), 0, "duplicate batch has no winners");
}

void TestBatchMarkKeepsWordsIndependent()
{
    RegionPlaneFixture fixture;
    fixture.markPlane[1] = 1ULL << 1;

    ExpectEq(fixture.region.MarkBitmapWord(1, (1ULL << 1) | (1ULL << 9)),
             1ULL << 9,
             "second word winners");
    ExpectEq(fixture.markPlane[0], 0, "first word unchanged");
    ExpectEq(fixture.markPlane[1], (1ULL << 1) | (1ULL << 9), "second word bitmap");
}

void TestMarkedProbeUsesResolvedWordAndBit()
{
    RegionPlaneFixture fixture;
    fixture.markPlane[0] = 1ULL << 5;
    fixture.markPlane[1] = 1ULL << 9;

    ExpectEq(fixture.region.IsMarkBitSet(0, 1ULL << 5), 1, "marked probe hit");
    ExpectEq(fixture.region.IsMarkBitSet(0, 1ULL << 6), 0, "marked probe miss");
    ExpectEq(fixture.region.IsMarkBitSet(1, 1ULL << 9), 1, "second word marked probe hit");
    ExpectEq(fixture.region.IsMarkBitSet(
                 MapleRuntime::MSBitmapArena::kMaxBitmapWords, 1ULL << 1),
             0,
             "out-of-range word rejected");
    ExpectEq(fixture.region.IsMarkBitSet(0, 0), 0, "empty mask rejected");
}

void TestResolvedBitmapAccessUsesValidatedCoordinates()
{
    RegionPlaneFixture fixture;
    fixture.markPlane[0] = 1ULL << 2;

    ExpectEq(fixture.region.IsMarkBitSetResolved(0, 1ULL << 2), 1,
             "resolved marked probe hit");
    ExpectEq(fixture.region.IsMarkBitSetResolved(0, 1ULL << 7), 0,
             "resolved marked probe miss");
    ExpectEq(fixture.region.MarkBitmapWordResolved(0, (1ULL << 2) | (1ULL << 7)),
             1ULL << 7,
             "resolved mark returns only the winning bit");
    ExpectEq(fixture.markPlane[0], (1ULL << 2) | (1ULL << 7),
             "resolved mark updates the validated bitmap word");
}

void TestDirectRegionLookupReturnsInfoAndOffsetFromOneHeapCoordinate()
{
    constexpr uintptr_t heapStart = 0x10000000;
    alignas(64) MSRegionInfo regions[3] {};
    MSRegionInfoArray::Init(heapStart, 3, regions);

    size_t regionOffset = 0;
    uintptr_t address = heapStart + MapleRuntime::MS_REGION_SIZE + 137;
    MSRegionInfo* info = MSRegionInfoArray::GetRegionInfoDirect(address, regionOffset);

    if (info != &regions[1]) {
        std::fprintf(stderr, "direct lookup returned the wrong RegionInfo\n");
        std::abort();
    }
    ExpectEq(regionOffset, 137, "direct lookup region offset");

    address = heapStart + 2 * MapleRuntime::MS_REGION_SIZE - 1;
    info = MSRegionInfoArray::GetRegionInfoDirect(address, regionOffset);
    if (info != &regions[1]) {
        std::fprintf(stderr, "last byte of a Region must not map to its successor\n");
        std::abort();
    }
    ExpectEq(regionOffset, MapleRuntime::MS_REGION_SIZE - 1,
             "direct lookup last-byte offset");

    MSRegionInfoArray::Cleanup();
}

void TestPendingPublicationSurvivesOwnerRelease()
{
    RegionPlaneFixture fixture;
    fixture.region.SetScanState(MSScanState::OWNED);

    fixture.region.PublishPendingMarkWord(3);
    if (!fixture.region.CasScanState(MSScanState::OWNED, MSScanState::IDLE)) {
        std::fprintf(stderr, "owner release failed\n");
        std::abort();
    }

    // A concurrent producer may have observed OWNED and skipped Region-token
    // publication. Its summary must remain durable across OWNED -> IDLE so the
    // post-release check or MarkTermination reconciliation can republish it.
    if (fixture.region.GetScanState() != MSScanState::IDLE) {
        std::fprintf(stderr, "owner release did not reach IDLE\n");
        std::abort();
    }
    ExpectEq(fixture.region.TakePendingMarkWords(), 1ULL << 3,
             "pending word survives owner release");
}

} // namespace

int main()
{
    TestBatchMarkReturnsOnlyNewBits();
    TestBatchMarkKeepsWordsIndependent();
    TestMarkedProbeUsesResolvedWordAndBit();
    TestResolvedBitmapAccessUsesValidatedCoordinates();
    TestDirectRegionLookupReturnsInfoAndOffsetFromOneHeapCoordinate();
    TestPendingPublicationSurvivesOwnerRelease();
    return 0;
}
