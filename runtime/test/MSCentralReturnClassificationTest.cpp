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

using MapleRuntime::MSCentralReturnDecision;
using MapleRuntime::MSCentralReturnKind;
using MapleRuntime::MSRegionInfo;
using MapleRuntime::MSRegionType;
using MapleRuntime::MSSweepState;
using MapleRuntime::MS_REGION_SIZE;

constexpr uint8_t kTestSizeClass = 1;
constexpr uint8_t kBumpReturnSizeClass = 6; // 80-byte slots, 409 slots in 7 words.

void ExpectTrue(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void ExpectEqU8(uint8_t actual, uint8_t expected, const char* message)
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

void ExpectEqU64(uint64_t actual, uint64_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr,
                     "%s: expected=0x%llx actual=0x%llx\n",
                     message,
                     static_cast<unsigned long long>(expected),
                     static_cast<unsigned long long>(actual));
        std::abort();
    }
}

void ExpectEqKind(MSCentralReturnKind actual, MSCentralReturnKind expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n",
                     message,
                     static_cast<unsigned>(expected),
                     static_cast<unsigned>(actual));
        std::abort();
    }
}

void ExpectEqType(MSRegionType actual, MSRegionType expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n",
                     message,
                     static_cast<unsigned>(expected),
                     static_cast<unsigned>(actual));
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

        size_t slotSize = MapleRuntime::SizeClassInfo::ClassToSize(sizeClass);
        size_t slotCount = MS_REGION_SIZE / slotSize;
        size_t wordCount = (slotCount + 63) / 64;

        allocWords.assign(wordCount, ~0ULL);
        size_t trailingBits = slotCount % 64;
        if (trailingBits != 0) {
            allocWords.back() = (1ULL << trailingBits) - 1;
        }
        markWords.assign(wordCount, 0);

        region.allocPtr = reinterpret_cast<uintptr_t>(payload);
        region.regionEnd = reinterpret_cast<uintptr_t>(payload) + MS_REGION_SIZE;
        region.totalSlotCount = static_cast<uint16_t>(slotCount);
        region.freeSlotCount = static_cast<uint16_t>(slotCount);
        region.sizeClass = sizeClass;
        region.sweepState = MSSweepState::UNSWEPT;
        region.regionType = MSRegionType::MS_REGION_BUMP;
        region.flags_ = 0;
        region.sweepGen = 0;
        region.reserved = 0;
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

void TestPrepareForCentralReturnMaterializesPartiallyUsedBumpBitmap()
{
    RegionFixture fixture(kBumpReturnSizeClass);
    constexpr size_t kAllocatedSlots = 65;
    size_t slotSize = MapleRuntime::SizeClassInfo::ClassToSize(kBumpReturnSizeClass);
    uintptr_t regionStart = reinterpret_cast<uintptr_t>(fixture.payload);

    for (size_t i = 0; i < kAllocatedSlots; ++i) {
        ExpectEqSize(fixture.region.AllocBump(), regionStart + i * slotSize, "bump allocation address");
    }

    MSCentralReturnDecision decision = fixture.region.PrepareForCentralReturn(kBumpReturnSizeClass);

    ExpectEqKind(decision.kind, MSCentralReturnKind::PARTIAL, "partially used bump should route to partial");
    ExpectEqU64(fixture.allocWords[0], 0, "returned partial must mark the first 64 slots allocated");
    ExpectEqU64(fixture.allocWords[1], ~1ULL, "returned partial must keep slot 64 allocated");
    for (size_t wordIdx = 2; wordIdx + 1 < fixture.allocWords.size(); ++wordIdx) {
        ExpectEqU64(fixture.allocWords[wordIdx], ~0ULL, "remaining whole words must stay free");
    }
    ExpectEqU64(fixture.allocWords.back(), (1ULL << 25) - 1, "invalid trailing bits must stay masked");
    ExpectEqU64(fixture.region.reserved, 0x7E, "summary must include only bitmap words with free slots");

    uintptr_t next = fixture.region.AllocFromBitmapFast();
    ExpectEqSize(next, regionStart + kAllocatedSlots * slotSize,
                 "bitmap allocation must continue after the bump-allocated prefix");
}

void TestPrepareForCentralReturnNormalizesReusableBumpRegion()
{
    RegionFixture fixture;
    fixture.region.freeSlotCount = 17;
    size_t slotSize = MapleRuntime::SizeClassInfo::ClassToSize(kTestSizeClass);
    fixture.region.allocPtr = reinterpret_cast<uintptr_t>(fixture.payload) +
        (fixture.region.totalSlotCount - fixture.region.freeSlotCount) * slotSize;

    MSCentralReturnDecision decision = fixture.region.PrepareForCentralReturn(/* callerCls */ 2);

    ExpectEqKind(decision.kind, MSCentralReturnKind::PARTIAL, "reusable bump region should route to partial");
    ExpectEqType(fixture.region.regionType,
                 MSRegionType::MS_REGION_PARTIAL,
                 "reusable bump region should stop returning as BUMP");
    ExpectEqU8(decision.bucketSizeClass, kTestSizeClass, "return bucket should follow region sizeClass");
    ExpectTrue(decision.callerClassMismatch, "mismatched caller cls should be reported");
}

void TestPrepareForCentralReturnNormalizesFullRegion()
{
    RegionFixture fixture;
    fixture.region.freeSlotCount = 0;
    fixture.region.allocPtr = fixture.region.regionEnd;

    MSCentralReturnDecision decision = fixture.region.PrepareForCentralReturn(kTestSizeClass);

    ExpectEqKind(decision.kind, MSCentralReturnKind::FULL, "full region should route to full");
    ExpectEqType(fixture.region.regionType, MSRegionType::MS_REGION_FULL, "full region type");
    ExpectEqU8(decision.bucketSizeClass, kTestSizeClass, "full region bucket sizeClass");
    ExpectTrue(!decision.callerClassMismatch, "matching caller cls should not report mismatch");
}

void TestPrepareForCentralReturnNormalizesEmptyRegion()
{
    RegionFixture fixture;
    fixture.region.freeSlotCount = fixture.region.totalSlotCount;

    MSCentralReturnDecision decision = fixture.region.PrepareForCentralReturn(kTestSizeClass);

    ExpectEqKind(decision.kind, MSCentralReturnKind::EMPTY_CACHE, "empty region should route to empty cache");
    ExpectEqType(fixture.region.regionType,
                 MSRegionType::MS_REGION_EMPTY_CACHE,
                 "empty region should be normalized before central return");
}

} // namespace

int main()
{
    TestPrepareForCentralReturnMaterializesPartiallyUsedBumpBitmap();
    TestPrepareForCentralReturnNormalizesReusableBumpRegion();
    TestPrepareForCentralReturnNormalizesFullRegion();
    TestPrepareForCentralReturnNormalizesEmptyRegion();
    return 0;
}
