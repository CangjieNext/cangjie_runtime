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
#define RTLOG_FATAL 0
#define RTLOG_INFO 1
#define RTLOG_ERROR 2
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

namespace MapleRuntime {
uint32_t SizeClassInfo::slotDivMagic_[kNumSizeClasses] = {};

uint64_t* MSBitmapArena::GetSlotBase(size_t)
{
    return nullptr;
}
} // namespace MapleRuntime

#include "src/Heap/MAllocator/MSRegionInfo.cpp"

namespace {

using MapleRuntime::MSRegionInfo;

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
    uint64_t bitmap[] = { 1ULL << 3, 0 };
    MSRegionInfo region {};
    region.markBitmap = bitmap;
    region.totalSlotCount = 128;

    uint64_t mask = (1ULL << 3) | (1ULL << 7) | (1ULL << 11);
    uint64_t newBits = region.MarkBitmapWord(0, mask);

    ExpectEq(newBits, (1ULL << 7) | (1ULL << 11), "batch mark winners");
    ExpectEq(bitmap[0], mask, "batch mark bitmap");
    ExpectEq(region.MarkBitmapWord(0, mask), 0, "duplicate batch has no winners");
}

void TestBatchMarkKeepsWordsIndependent()
{
    uint64_t bitmap[] = { 0, 1ULL << 1 };
    MSRegionInfo region {};
    region.markBitmap = bitmap;
    region.totalSlotCount = 128;

    ExpectEq(region.MarkBitmapWord(1, (1ULL << 1) | (1ULL << 9)),
             1ULL << 9,
             "second word winners");
    ExpectEq(bitmap[0], 0, "first word unchanged");
    ExpectEq(bitmap[1], (1ULL << 1) | (1ULL << 9), "second word bitmap");
}

void TestMarkedProbeUsesResolvedWordAndBit()
{
    uint64_t bitmap[] = { 1ULL << 5, 1ULL << 9 };
    MSRegionInfo region {};
    region.markBitmap = bitmap;
    region.totalSlotCount = 128;

    ExpectEq(region.IsMarkBitSet(0, 1ULL << 5), 1, "marked probe hit");
    ExpectEq(region.IsMarkBitSet(0, 1ULL << 6), 0, "marked probe miss");
    ExpectEq(region.IsMarkBitSet(1, 1ULL << 9), 1, "second word marked probe hit");
    ExpectEq(region.IsMarkBitSet(2, 1ULL << 1), 0, "out-of-range word rejected");
    ExpectEq(region.IsMarkBitSet(0, 0), 0, "empty mask rejected");

    region.markBitmap = nullptr;
    ExpectEq(region.IsMarkBitSet(0, 1ULL << 5), 0, "null bitmap rejected");
}

} // namespace

int main()
{
    TestBatchMarkReturnsOnlyNewBits();
    TestBatchMarkKeepsWordsIndependent();
    TestMarkedProbeUsesResolvedWordAndBit();
    return 0;
}
