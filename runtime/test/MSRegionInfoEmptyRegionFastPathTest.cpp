#include <algorithm>
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
#define RTLOG_FATAL 0
#define RTLOG_INFO 1
#define RTLOG_ERROR 2
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

namespace MapleRuntime {
uint32_t SizeClassInfo::slotDivMagic_[kNumSizeClasses] = {};

uint64_t* MSBitmapArena::GetSlotBase(size_t)
{
    return nullptr;
}
} // namespace MapleRuntime

#include "src/Heap/MAllocator/MSRegionInfo.cpp"

namespace {

using MapleRuntime::MS_REGION_SIZE;
using MapleRuntime::MSRegionInfo;
using MapleRuntime::MSRegionType;
using MapleRuntime::MSSweepState;

constexpr uint8_t kTestSizeClass = 5; // 64-byte slots, no trailing bitmap bits.
constexpr uint8_t kTrailingSizeClass = 6; // 80-byte slots, 409 valid bits in 7 words.
constexpr uint64_t kAllocBitmapPoison = 0x0123456789ABCDEFULL;
constexpr uint64_t kSummaryPoison = 0xDEADBEEFCAFEBABEULL;

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

void ExpectEq64(uint64_t actual, uint64_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=0x%llx actual=0x%llx\n",
                     message,
                     static_cast<unsigned long long>(expected),
                     static_cast<unsigned long long>(actual));
        std::abort();
    }
}

struct RegionFixture {
    void* payload = nullptr;
    std::vector<uint64_t> allocWords;
    std::vector<uint64_t> markWords;
    MSRegionInfo region {};

    explicit RegionFixture(uint8_t sizeClass = kTestSizeClass)
    {
        payload = std::malloc(MS_REGION_SIZE);
        if (payload == nullptr) {
            std::fprintf(stderr, "malloc failed\n");
            std::abort();
        }
        std::memset(payload, 0xA5, MS_REGION_SIZE);

        size_t slotSize = MapleRuntime::SizeClassInfo::ClassToSize(sizeClass);
        size_t slotCount = MS_REGION_SIZE / slotSize;
        size_t wordCount = (slotCount + 63) / 64;

        allocWords.assign(wordCount, kAllocBitmapPoison);
        markWords.assign(wordCount, 0);

        region.allocPtr = reinterpret_cast<uintptr_t>(payload);
        region.regionEnd = reinterpret_cast<uintptr_t>(payload) + MS_REGION_SIZE;
        region.totalSlotCount = static_cast<uint16_t>(slotCount);
        region.freeSlotCount = 0;
        region.sizeClass = sizeClass;
        region.sweepState = MSSweepState::UNSWEPT;
        region.regionType = MSRegionType::MS_REGION_PARTIAL;
        region.flags_ = 0;
        region.sweepGen = 0;
        region.reserved = kSummaryPoison;
        region.allocBitmap = allocWords.data();
        region.markBitmap = markWords.data();
        region.nextRegionIdx = MSRegionInfo::INVALID_IDX;
        region.prevRegionIdx = MSRegionInfo::INVALID_IDX;
    }

    ~RegionFixture()
    {
        std::free(payload);
    }
};

void ExpectPayloadZeroed(const RegionFixture& fixture)
{
    auto* bytes = static_cast<const unsigned char*>(fixture.payload);
    for (size_t i = 0; i < MS_REGION_SIZE; ++i) {
        if (bytes[i] != 0) {
            std::fprintf(stderr, "payload not zeroed at offset=%zu value=0x%x\n", i, bytes[i]);
            std::abort();
        }
    }
}

void ExpectAllocBitmapPoisoned(const RegionFixture& fixture, const char* message)
{
    for (size_t i = 0; i < fixture.allocWords.size(); ++i) {
        if (fixture.allocWords[i] != kAllocBitmapPoison) {
            std::fprintf(stderr,
                         "%s: word[%zu] expected=0x%llx actual=0x%llx\n",
                         message,
                         i,
                         static_cast<unsigned long long>(kAllocBitmapPoison),
                         static_cast<unsigned long long>(fixture.allocWords[i]));
            std::abort();
        }
    }
}

void ExpectMarkBitmapZero(const RegionFixture& fixture, const char* message)
{
    for (size_t i = 0; i < fixture.markWords.size(); ++i) {
        if (fixture.markWords[i] != 0) {
            std::fprintf(stderr,
                         "%s: word[%zu] expected=0 actual=0x%llx\n",
                         message,
                         i,
                         static_cast<unsigned long long>(fixture.markWords[i]));
            std::abort();
        }
    }
}

void TestSweepRegionAllDeadSkipsSweptBitmapConstruction()
{
    RegionFixture fixture;

    size_t freedBytes = fixture.region.SweepRegion();

    ExpectEq(freedBytes, MS_REGION_SIZE, "empty sweep should free full region");
    ExpectEq(fixture.region.freeSlotCount, fixture.region.totalSlotCount, "empty sweep free slots");
    ExpectTrue(fixture.region.sweepState == MSSweepState::SWEPT, "empty sweep should enter SWEPT state");
    ExpectEq64(fixture.region.reserved, 0, "empty sweep should not build summary");
    ExpectAllocBitmapPoisoned(fixture, "empty sweep should not rewrite alloc bitmap");
    ExpectMarkBitmapZero(fixture, "empty sweep should keep mark bitmap clear");
    ExpectPayloadZeroed(fixture);
}

void TestResetToBumpAfterEmptySweepPreservesBitmapState()
{
    RegionFixture fixture;
    fixture.region.freeSlotCount = fixture.region.totalSlotCount;
    fixture.region.sweepState = MSSweepState::SWEPT;
    fixture.region.regionType = MSRegionType::MS_REGION_PARTIAL;
    fixture.region.reserved = 0;

    fixture.region.ResetToBump();

    ExpectEq(fixture.region.allocPtr, reinterpret_cast<uintptr_t>(fixture.payload), "reset-to-bump allocPtr");
    ExpectEq(fixture.region.freeSlotCount, fixture.region.totalSlotCount, "reset-to-bump free slots");
    ExpectTrue(fixture.region.sweepState == MSSweepState::UNSWEPT,
               "reset-to-bump should leave region in UNSWEPT state");
    ExpectTrue(fixture.region.regionType == MSRegionType::MS_REGION_BUMP,
               "reset-to-bump should leave region in BUMP state");
    ExpectEq64(fixture.region.reserved, 0, "reset-to-bump should not rebuild summary for empty region");
    ExpectAllocBitmapPoisoned(fixture, "reset-to-bump should preserve alloc bitmap for swept-empty region");
    ExpectMarkBitmapZero(fixture, "reset-to-bump should preserve mark bitmap for swept-empty region");
}

void SetAllValidMarkBits(RegionFixture& fixture)
{
    std::fill(fixture.markWords.begin(), fixture.markWords.end(), ~0ULL);
    size_t trailingBits = fixture.region.totalSlotCount % 64;
    if (trailingBits != 0) {
        fixture.markWords.back() = (1ULL << trailingBits) - 1;
    }
}

void ClearMarkRange(RegionFixture& fixture, size_t startSlot, size_t endSlot)
{
    for (size_t slot = startSlot; slot < endSlot; ++slot) {
        fixture.markWords[slot / 64] &= ~(1ULL << (slot % 64));
    }
}

bool IsDeadTestSlot(size_t slot)
{
    return (slot >= 62 && slot < 67) || (slot >= 407 && slot < 409);
}

void TestSweepRegionDeadRunsCrossWordsAndMaskTrailingBits()
{
    RegionFixture fixture(kTrailingSizeClass);
    SetAllValidMarkBits(fixture);
    ClearMarkRange(fixture, 62, 67);   // One dead run spanning bitmap words 0 and 1.
    ClearMarkRange(fixture, 407, 409); // One dead run ending at totalSlotCount.
    const std::vector<uint64_t> originalMarks = fixture.markWords;

    size_t slotSize = MapleRuntime::SizeClassInfo::ClassToSize(kTrailingSizeClass);
    size_t freedBytes = fixture.region.SweepRegion();

    ExpectEq(freedBytes, 7 * slotSize, "partial sweep freed bytes");
    ExpectEq(fixture.region.freeSlotCount, 7, "partial sweep free slots");
    ExpectTrue(fixture.region.sweepState == MSSweepState::SWEPT,
               "partial sweep should enter SWEPT state");
    ExpectEq64(fixture.region.reserved, (1ULL << 0) | (1ULL << 1) | (1ULL << 6),
               "partial sweep summary");
    ExpectTrue(fixture.markWords == originalMarks, "sweep must not clear or rewrite mark bitmap");

    auto* bytes = static_cast<const unsigned char*>(fixture.payload);
    for (size_t slot = 0; slot < fixture.region.totalSlotCount; ++slot) {
        unsigned char expected = IsDeadTestSlot(slot) ? 0 : 0xA5;
        for (size_t byte = 0; byte < slotSize; ++byte) {
            if (bytes[slot * slotSize + byte] != expected) {
                std::fprintf(stderr,
                             "slot payload mismatch: slot=%zu byte=%zu expected=0x%x actual=0x%x\n",
                             slot,
                             byte,
                             expected,
                             bytes[slot * slotSize + byte]);
                std::abort();
            }
        }
    }

    size_t payloadBytes = fixture.region.totalSlotCount * slotSize;
    for (size_t offset = payloadBytes; offset < MS_REGION_SIZE; ++offset) {
        if (bytes[offset] != 0xA5) {
            std::fprintf(stderr, "trailing payload was cleared at offset=%zu\n", offset);
            std::abort();
        }
    }

    for (size_t word = 0; word < fixture.allocWords.size(); ++word) {
        uint64_t expected = 0;
        for (size_t bit = 0; bit < 64; ++bit) {
            size_t slot = word * 64 + bit;
            if (slot < fixture.region.totalSlotCount && IsDeadTestSlot(slot)) {
                expected |= 1ULL << bit;
            }
        }
        ExpectEq64(fixture.allocWords[word], expected, "partial sweep allocation bitmap");
    }
}

void TestLargeRegionUsesDistinctObjectAndSpanSemantics()
{
    MSRegionInfo region {};
    constexpr uintptr_t start = 0x10000000;
    constexpr size_t spanBytes = 4 * MS_REGION_SIZE;
    constexpr size_t objectBytes = 3 * MS_REGION_SIZE + 123;

    region.InitForLargeObject(start, start + spanBytes, objectBytes);

    ExpectEq(region.LargeObjectStart(), start, "large object start");
    ExpectEq(region.LargeObjectSize(), objectBytes, "large logical object bytes");
    ExpectEq(region.LargeSpanBytes(), spanBytes, "large span bytes");
    ExpectEq(region.LargeSpanCount(), 4, "large span Region count");
}

} // namespace

int main()
{
    TestSweepRegionAllDeadSkipsSweptBitmapConstruction();
    TestResetToBumpAfterEmptySweepPreservesBitmapState();
    TestSweepRegionDeadRunsCrossWordsAndMaskTrailingBits();
    TestLargeRegionUsesDistinctObjectAndSpanSemantics();
    return 0;
}
