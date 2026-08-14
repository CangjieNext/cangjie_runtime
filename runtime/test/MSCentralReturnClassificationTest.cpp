#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

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
            std::fprintf(stderr, "CHECK_DETAIL failed: %s:%d: %s\n", __FILE__, __LINE__, #x);   \
            std::abort();                                                                        \
        }                                                                                        \
    } while (false)
#define MRT_ASSERT(x, msg)                                                                       \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::fprintf(stderr, "MRT_ASSERT failed: %s:%d: %s\n", __FILE__, __LINE__, msg);    \
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
extern "C" MSGCFastPathState CJ_MRT_MSGCFastPathState = {};

void MSBitmapArena::EnsurePlaneRangeCommitted(MSBitmapPlane, size_t, size_t)
{
}

uint64_t* MSBitmapArena::GetAllocPlane(size_t regionIdx)
{
    return reinterpret_cast<uint64_t*>(
        CJ_MRT_MSGCFastPathState.allocPlaneBase +
        regionIdx * kPlaneStrideBytes);
}

uint64_t* MSBitmapArena::GetMarkPlane(size_t regionIdx)
{
    return reinterpret_cast<uint64_t*>(
        CJ_MRT_MSGCFastPathState.markPlaneBase +
        regionIdx * kPlaneStrideBytes);
}

} // namespace MapleRuntime

#include "src/Heap/MAllocator/MSRegionInfo.cpp"

namespace {

using MapleRuntime::MS_REGION_SIZE;
using MapleRuntime::MSAllocMode;
using MapleRuntime::MSBitmapArena;
using MapleRuntime::MSCentralReturnDecision;
using MapleRuntime::MSCentralReturnKind;
using MapleRuntime::MSRegionInfo;
using MapleRuntime::MSScanClass;

constexpr uint8_t kTestSizeClass = 1;
constexpr uint8_t kBumpReturnSizeClass = 6; // 80-byte slots, 409 slots in 7 words.

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
        std::fprintf(
            stderr, "%s: expected=%zu actual=%zu\n",
            message, expected, actual);
        std::abort();
    }
}

void ExpectEq64(uint64_t actual, uint64_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(
            stderr, "%s: expected=0x%llx actual=0x%llx\n",
            message, static_cast<unsigned long long>(expected),
            static_cast<unsigned long long>(actual));
        std::abort();
    }
}

struct RegionFixture {
    void* payload = nullptr;
    alignas(64) MSRegionInfo region {};
    alignas(64) std::array<uint64_t, MSBitmapArena::kMaxBitmapWords> allocWords {};
    alignas(64) std::array<uint64_t, MSBitmapArena::kMaxBitmapWords> markWords {};
    alignas(64) std::array<uint64_t, MSBitmapArena::kMaxBitmapWords> scanWords {};

    explicit RegionFixture(uint8_t sizeClass = kTestSizeClass)
    {
        payload = std::malloc(MS_REGION_SIZE);
        if (payload == nullptr) {
            std::fprintf(stderr, "malloc failed\n");
            std::abort();
        }

        MapleRuntime::MSRegionInfoArray::Init(
            reinterpret_cast<uintptr_t>(payload), 1, &region);
        MapleRuntime::CJ_MRT_MSGCFastPathState.heapReservedStart =
            reinterpret_cast<uintptr_t>(payload);
        MapleRuntime::CJ_MRT_MSGCFastPathState.heapReservedEnd =
            reinterpret_cast<uintptr_t>(payload) + MS_REGION_SIZE;
        MapleRuntime::CJ_MRT_MSGCFastPathState.allocPlaneBase =
            reinterpret_cast<uintptr_t>(allocWords.data());
        MapleRuntime::CJ_MRT_MSGCFastPathState.markPlaneBase =
            reinterpret_cast<uintptr_t>(markWords.data());
        MapleRuntime::CJ_MRT_MSGCFastPathState.scanPlaneBase =
            reinterpret_cast<uintptr_t>(scanWords.data());
        region.InitForSizeClass(sizeClass, MSScanClass::HAS_REF);
    }

    ~RegionFixture()
    {
        std::free(payload);
    }
};

void TestPartiallyUsedBumpMaterializesBitmap()
{
    RegionFixture fixture(kBumpReturnSizeClass);
    constexpr size_t kAllocatedSlots = 65;
    uintptr_t regionStart = fixture.region.RegionStart();
    size_t slotSize = fixture.region.GetSlotSize();

    for (size_t i = 0; i < kAllocatedSlots; ++i) {
        ExpectEq(
            fixture.region.AllocBump(),
            regionStart + i * slotSize,
            "bump allocation address");
    }

    MSCentralReturnDecision decision =
        fixture.region.PrepareForCentralReturn(kBumpReturnSizeClass);

    ExpectTrue(
        decision.kind == MSCentralReturnKind::PARTIAL,
        "partially used BUMP must route to PARTIAL");
    ExpectEq(
        decision.freeSlots,
        fixture.region.GetTotalSlotCount() - kAllocatedSlots,
        "partially used BUMP free slots");
    ExpectEq64(fixture.allocWords[0], 0, "first 64 slots allocated");
    ExpectEq64(fixture.allocWords[1], ~UINT64_C(1), "slot 64 allocated");
    ExpectEq64(
        fixture.region.allocSummary, UINT64_C(0x7E),
        "summary contains only words with free slots");
}

void TestCallerMismatchUsesRegionClassAndIgnoresTombstone()
{
    RegionFixture fixture;
    fixture.region.AllocBump();
    fixture.region.reserved32 = 0xDEADBEEF;

    MSCentralReturnDecision decision =
        fixture.region.PrepareForCentralReturn(/* callerCls */ 2);

    ExpectTrue(
        decision.kind == MSCentralReturnKind::PARTIAL,
        "reusable BUMP must route to PARTIAL");
    ExpectEq(
        decision.bucketSizeClass, kTestSizeClass,
        "return bucket follows Region sizeClass");
    ExpectTrue(
        decision.callerClassMismatch,
        "mismatched caller class must be reported");
    ExpectEq64(
        fixture.region.reserved32, 0xDEADBEEF,
        "legacy ABI tombstone must be ignored");
}

void TestBitmapReturnCanonicalizesSummary()
{
    RegionFixture fixture;
    fixture.region.SetAllocMode(MSAllocMode::BITMAP);
    std::fill(fixture.allocWords.begin(), fixture.allocWords.end(), 0);
    fixture.allocWords[2] = UINT64_C(1) << 9;
    fixture.region.allocSummary = ~UINT64_C(0);

    MSCentralReturnDecision decision =
        fixture.region.PrepareForCentralReturn(kTestSizeClass);

    ExpectTrue(
        decision.kind == MSCentralReturnKind::PARTIAL,
        "one free BITMAP slot must route to PARTIAL");
    ExpectEq(decision.freeSlots, 1, "BITMAP popcount result");
    ExpectEq64(
        fixture.region.allocSummary, UINT64_C(1) << 2,
        "BITMAP summary canonicalization");
}

void TestFullAndEmptyBumpClassification()
{
    RegionFixture full;
    full.region.allocPtr = full.region.regionEnd;
    MSCentralReturnDecision fullDecision =
        full.region.PrepareForCentralReturn(kTestSizeClass);
    ExpectTrue(
        fullDecision.kind == MSCentralReturnKind::FULL,
        "exhausted BUMP must route to FULL");
    ExpectEq(fullDecision.freeSlots, 0, "full BUMP free slots");

    RegionFixture empty;
    MSCentralReturnDecision emptyDecision =
        empty.region.PrepareForCentralReturn(kTestSizeClass);
    ExpectTrue(
        emptyDecision.kind == MSCentralReturnKind::EMPTY_CACHE,
        "unused BUMP must route to EMPTY_CACHE");
    ExpectEq(
        emptyDecision.freeSlots,
        empty.region.GetTotalSlotCount(),
        "empty BUMP free slots");
}

} // namespace

int main()
{
    TestPartiallyUsedBumpMaterializesBitmap();
    TestCallerMismatchUsesRegionClassAndIgnoresTombstone();
    TestBitmapReturnCanonicalizesSummary();
    TestFullAndEmptyBumpClassification();
    return 0;
}
