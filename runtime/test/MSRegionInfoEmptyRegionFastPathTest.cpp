#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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
using MapleRuntime::MSCentralReturnKind;
using MapleRuntime::MSRegionInfo;
using MapleRuntime::MSRegionKind;
using MapleRuntime::MSScanClass;
using MapleRuntime::SizeClassInfo;

constexpr uint8_t kTestSizeClass = 5;     // 64-byte slots.
constexpr uint8_t kTrailingSizeClass = 6; // 80-byte slots, 409 valid bits.
constexpr uint64_t kAllocBitmapPoison = 0x0123456789ABCDEFULL;

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
        region.allocPtr = region.regionEnd;
        region.SetAllocMode(MSAllocMode::BITMAP);
        std::fill(allocWords.begin(), allocWords.end(), kAllocBitmapPoison);
        std::fill(markWords.begin(), markWords.end(), 0);
        std::memset(payload, 0xA5, MS_REGION_SIZE);
    }

    ~RegionFixture()
    {
        std::free(payload);
    }
};

void ExpectPayloadZeroed(const RegionFixture& fixture)
{
    const auto* bytes = static_cast<const unsigned char*>(fixture.payload);
    for (size_t i = 0; i < MS_REGION_SIZE; ++i) {
        if (bytes[i] != 0) {
            std::fprintf(
                stderr, "payload not zeroed at offset=%zu value=0x%x\n",
                i, bytes[i]);
            std::abort();
        }
    }
}

void ExpectAllocBitmapPoisoned(
    const RegionFixture& fixture, const char* message)
{
    for (size_t i = 0; i < fixture.region.GetBitmapWordCount(); ++i) {
        if (fixture.allocWords[i] != kAllocBitmapPoison) {
            std::fprintf(
                stderr,
                "%s: word[%zu] expected=0x%llx actual=0x%llx\n",
                message, i,
                static_cast<unsigned long long>(kAllocBitmapPoison),
                static_cast<unsigned long long>(fixture.allocWords[i]));
            std::abort();
        }
    }
}

void TestAnyValidMarkBitProbeMasksTailAndStopsAtLiveWord()
{
    std::array<uint64_t, 3> words {};
    constexpr uint64_t kTailMask = UINT64_C(0x1FF);

    ExpectTrue(
        !MapleRuntime::HasAnyValidMarkBits(
            words.data(), words.size(), kTailMask),
        "an empty mark bitmap must not report a live slot");

    words[2] = UINT64_C(1) << 63;
    ExpectTrue(
        !MapleRuntime::HasAnyValidMarkBits(
            words.data(), words.size(), kTailMask),
        "invalid trailing bits must not report a live slot");

    words[2] = UINT64_C(1) << 8;
    ExpectTrue(
        MapleRuntime::HasAnyValidMarkBits(
            words.data(), words.size(), kTailMask),
        "a valid bit in the trailing word must report a live slot");

    words[2] = 0;
    words[0] = UINT64_C(1) << 41;
    ExpectTrue(
        MapleRuntime::HasAnyValidMarkBits(
            words.data(), words.size(), kTailMask),
        "a live bit in an earlier word must report a live slot");
}

void TestFullBumpReturnMaterializesAllocatedPlane()
{
    RegionFixture fixture;
    fixture.region.SetAllocMode(MSAllocMode::BUMP);
    fixture.region.allocPtr = fixture.region.regionEnd;
    fixture.region.allocSummary =
        (UINT64_C(1) << fixture.region.GetBitmapWordCount()) - 1;
    for (size_t word = 0; word < fixture.region.GetBitmapWordCount(); ++word) {
        fixture.allocWords[word] = ~UINT64_C(0);
    }
    fixture.allocWords[fixture.region.GetBitmapWordCount() - 1] =
        fixture.region.GetValidTailMask();

    auto decision =
        fixture.region.PrepareForCentralReturn(kTestSizeClass);

    ExpectTrue(
        decision.kind == MSCentralReturnKind::FULL,
        "full BUMP Region must route to FULL");
    ExpectEq(decision.freeSlots, 0, "full BUMP Region free slots");
    ExpectTrue(
        fixture.region.GetAllocMode() == MSAllocMode::BITMAP,
        "full BUMP Region must publish BITMAP mode");
    ExpectEq64(
        fixture.region.allocSummary, 0,
        "full BUMP Region must publish an empty free-word summary");
    for (size_t word = 0; word < fixture.region.GetBitmapWordCount(); ++word) {
        ExpectEq64(
            fixture.allocWords[word], 0,
            "full BUMP Region must mark every valid slot allocated");
    }
}

void TestPartialBumpReturnDerivesFreeSlotsAndIgnoresLegacyAbiTombstone()
{
    RegionFixture fixture;
    constexpr size_t kAllocatedSlots = 3;
    fixture.region.SetAllocMode(MSAllocMode::BUMP);
    fixture.region.allocPtr =
        fixture.region.RegionStart() +
        kAllocatedSlots * fixture.region.GetSlotSize();
    fixture.region.reserved32 = 0xDEADBEEF;

    auto decision =
        fixture.region.PrepareForCentralReturn(kTestSizeClass);

    ExpectTrue(
        decision.kind == MSCentralReturnKind::PARTIAL,
        "partial BUMP Region must route to PARTIAL");
    ExpectEq(
        decision.freeSlots,
        fixture.region.GetTotalSlotCount() - kAllocatedSlots,
        "partial BUMP free slots must be derived from allocPtr");
    ExpectEq64(
        fixture.region.reserved32, 0xDEADBEEF,
        "legacy ABI tombstone must not participate in classification");
    ExpectTrue(
        fixture.region.GetAllocMode() == MSAllocMode::BITMAP,
        "partial BUMP Region must publish BITMAP mode");
}

void TestBitmapReturnRepairsStaleNegativeSummary()
{
    RegionFixture fixture;
    std::fill(fixture.allocWords.begin(), fixture.allocWords.end(), 0);
    fixture.allocWords[1] = UINT64_C(1) << 7;
    fixture.region.allocSummary = 0;

    auto decision =
        fixture.region.PrepareForCentralReturn(kTestSizeClass);

    ExpectTrue(
        decision.kind == MSCentralReturnKind::PARTIAL,
        "one bitmap free slot must route to PARTIAL");
    ExpectEq(
        decision.freeSlots, 1,
        "BITMAP return must popcount the allocation plane");
    ExpectEq64(
        fixture.region.allocSummary, UINT64_C(1) << 1,
        "BITMAP return must repair a stale-negative summary");
}

void TestBitmapReturnRepairsStalePositiveSummary()
{
    RegionFixture fixture;
    std::fill(fixture.allocWords.begin(), fixture.allocWords.end(), 0);
    fixture.region.allocSummary = ~UINT64_C(0);

    auto decision =
        fixture.region.PrepareForCentralReturn(kTestSizeClass);

    ExpectTrue(
        decision.kind == MSCentralReturnKind::FULL,
        "zero bitmap free slots must route to FULL");
    ExpectEq(
        decision.freeSlots, 0,
        "full BITMAP Region must report zero free slots");
    ExpectEq64(
        fixture.region.allocSummary, 0,
        "BITMAP return must repair a stale-positive summary");
}

void TestSweepRegionAllDeadSkipsAllocPlaneConstruction()
{
    RegionFixture fixture;

    auto decision = fixture.region.SweepRegion();

    ExpectEq(
        decision.freeSlots,
        fixture.region.GetTotalSlotCount(),
        "empty sweep free slots");
    ExpectTrue(
        decision.kind == MSCentralReturnKind::EMPTY_CACHE,
        "empty sweep must route directly to EMPTY_CACHE");
    ExpectTrue(
        fixture.region.GetAllocMode() == MSAllocMode::BITMAP,
        "empty sweep must publish BITMAP mode");
    ExpectEq64(
        fixture.region.allocSummary, 0,
        "empty sweep must not build a bitmap summary");
    ExpectAllocBitmapPoisoned(
        fixture, "empty sweep must not rewrite alloc plane");
    ExpectPayloadZeroed(fixture);
}

void TestResetToBumpAfterEmptySweepPreservesPlaneState()
{
    RegionFixture fixture;
    auto decision = fixture.region.SweepRegion();
    ExpectTrue(
        decision.kind == MSCentralReturnKind::EMPTY_CACHE,
        "reset-to-bump fixture must first become sweep-empty");

    fixture.region.ResetToBump();

    ExpectEq(
        fixture.region.allocPtr,
        reinterpret_cast<uintptr_t>(fixture.payload),
        "reset-to-bump allocPtr");
    ExpectTrue(
        fixture.region.GetRegionKind() == MSRegionKind::SMALL &&
        fixture.region.GetAllocMode() == MSAllocMode::BUMP,
        "reset-to-bump small BUMP role");
    ExpectEq64(
        fixture.region.allocSummary, 0,
        "reset-to-bump keeps swept-empty summary");
    ExpectAllocBitmapPoisoned(
        fixture, "reset-to-bump preserves swept-empty alloc plane");
}

void SetAllValidMarkBits(RegionFixture& fixture)
{
    size_t words = fixture.region.GetBitmapWordCount();
    for (size_t i = 0; i < words; ++i) {
        fixture.markWords[i] = ~UINT64_C(0);
    }
    fixture.markWords[words - 1] =
        fixture.region.GetValidTailMask();
}

void ClearMarkRange(
    RegionFixture& fixture, size_t startSlot, size_t endSlot)
{
    for (size_t slot = startSlot; slot < endSlot; ++slot) {
        fixture.markWords[slot / 64] &=
            ~(UINT64_C(1) << (slot % 64));
    }
}

bool IsDeadTestSlot(size_t slot)
{
    return (slot >= 62 && slot < 67) ||
        (slot >= 407 && slot < 409);
}

void TestSweepRegionDeadRunsCrossWordsAndMaskTrailingBits()
{
    RegionFixture fixture(kTrailingSizeClass);
    SetAllValidMarkBits(fixture);
    ClearMarkRange(fixture, 62, 67);
    ClearMarkRange(fixture, 407, 409);
    auto originalMarks = fixture.markWords;

    size_t slotSize = SizeClassInfo::ClassToSize(kTrailingSizeClass);
    auto decision = fixture.region.SweepRegion();

    ExpectEq(
        decision.freeSlots, 7,
        "partial sweep free slots");
    ExpectTrue(
        decision.kind == MSCentralReturnKind::PARTIAL,
        "partial sweep must route directly to PARTIAL");
    ExpectEq(
        decision.freeSlots * slotSize, 7 * slotSize,
        "partial sweep freed bytes");
    ExpectEq64(
        fixture.region.allocSummary,
        (UINT64_C(1) << 0) | (UINT64_C(1) << 1) |
            (UINT64_C(1) << 6),
        "partial sweep summary");
    ExpectTrue(
        fixture.markWords == originalMarks,
        "sweep must not rewrite mark plane");

    const auto* bytes =
        static_cast<const unsigned char*>(fixture.payload);
    for (size_t slot = 0;
         slot < fixture.region.GetTotalSlotCount(); ++slot) {
        unsigned char expected = IsDeadTestSlot(slot) ? 0 : 0xA5;
        for (size_t byte = 0; byte < slotSize; ++byte) {
            if (bytes[slot * slotSize + byte] != expected) {
                std::fprintf(
                    stderr,
                    "slot payload mismatch: slot=%zu byte=%zu "
                    "expected=0x%x actual=0x%x\n",
                    slot, byte, expected,
                    bytes[slot * slotSize + byte]);
                std::abort();
            }
        }
    }

    for (size_t word = 0;
         word < fixture.region.GetBitmapWordCount(); ++word) {
        uint64_t expected = 0;
        for (size_t bit = 0; bit < 64; ++bit) {
            size_t slot = word * 64 + bit;
            if (slot < fixture.region.GetTotalSlotCount() &&
                IsDeadTestSlot(slot)) {
                expected |= UINT64_C(1) << bit;
            }
        }
        ExpectEq64(
            fixture.allocWords[word], expected,
            "partial sweep alloc plane");
    }
}

void TestLargeRegionUsesDistinctObjectAndSpanSemantics()
{
    MSRegionInfo region {};
    constexpr uintptr_t start = 0x10000000;
    constexpr size_t spanBytes = 4 * MS_REGION_SIZE;
    constexpr size_t objectBytes = 3 * MS_REGION_SIZE + 123;

    region.InitForLargeObject(
        start, start + spanBytes, objectBytes,
        MSScanClass::HAS_REF);

    ExpectEq(region.LargeObjectStart(), start, "large object start");
    ExpectEq(
        region.LargeObjectSize(), objectBytes,
        "large logical object bytes");
    ExpectEq(region.LargeSpanBytes(), spanBytes, "large span bytes");
    ExpectEq(region.LargeSpanCount(), 4, "large span count");
    ExpectTrue(
        region.GetRegionKind() == MSRegionKind::LARGE &&
        region.GetScanClass() == MSScanClass::HAS_REF,
        "large primary role and ScanClass");
}

} // namespace

int main()
{
    TestAnyValidMarkBitProbeMasksTailAndStopsAtLiveWord();
    TestFullBumpReturnMaterializesAllocatedPlane();
    TestPartialBumpReturnDerivesFreeSlotsAndIgnoresLegacyAbiTombstone();
    TestBitmapReturnRepairsStaleNegativeSummary();
    TestBitmapReturnRepairsStalePositiveSummary();
    TestSweepRegionAllDeadSkipsAllocPlaneConstruction();
    TestResetToBumpAfterEmptySweepPreservesPlaneState();
    TestSweepRegionDeadRunsCrossWordsAndMaskTrailingBits();
    TestLargeRegionUsesDistinctObjectAndSpanSemantics();
    return 0;
}
