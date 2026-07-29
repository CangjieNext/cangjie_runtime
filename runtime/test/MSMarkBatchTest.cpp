#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define MRT_USE_MARK_SWEEP 1
#define MRT_BASE_LOG_H

#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#define DCHECK(...)
#define CHECK_DETAIL(x, ...)                                                                     \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::abort();                                                                        \
        }                                                                                        \
    } while (false)

#include "src/Heap/MCollector/MSMarkBatch.h"

namespace {

using MapleRuntime::MSMarkWordBatch;
using MapleRuntime::MSMarkWordBatchEntry;

void Expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void TestCoalescesSameBitmapWord()
{
    MSMarkWordBatch batch;
    size_t flushes = 0;
    auto flush = [&flushes](const MSMarkWordBatchEntry&) {
        ++flushes;
    };

    batch.Add(7, 3, 1ULL << 1, flush);
    batch.Add(7, 3, 1ULL << 9, flush);
    Expect(batch.Size() == 1, "same word must share one batch entry");

    uint64_t observedMask = 0;
    batch.FlushAll([&](const MSMarkWordBatchEntry& entry) {
        ++flushes;
        Expect(entry.regionIdx == 7 && entry.wordIdx == 3,
               "coalesced entry coordinate changed");
        observedMask = entry.mask;
    });
    Expect(observedMask == ((1ULL << 1) | (1ULL << 9)),
           "coalesced mask lost a candidate bit");
    Expect(flushes == 1 && batch.IsEmpty(),
           "FlushAll must empty the coalesced batch");
}

void TestEvictsOneEntryAtCapacity()
{
    MSMarkWordBatch batch;
    std::vector<uint32_t> flushed;
    auto flush = [&flushed](const MSMarkWordBatchEntry& entry) {
        flushed.push_back(entry.regionIdx);
    };

    for (size_t i = 0; i < MSMarkWordBatch::kCapacity; ++i) {
        batch.Add(static_cast<uint32_t>(i), 0, 1, flush);
    }
    Expect(flushed.empty(), "a non-full batch must not evict");
    batch.Add(100, 0, 2, flush);
    Expect(flushed.size() == 1,
           "full batch must evict exactly one set-local victim");
    Expect(batch.Size() == MSMarkWordBatch::kCapacity,
           "eviction must replace instead of growing the batch");

    batch.FlushAll(flush);
    Expect(flushed.size() == MSMarkWordBatch::kCapacity + 1,
           "FlushAll did not flush every resident entry");
}

void TestCachesOneMarkEpochForBatchLifetime()
{
    MSMarkWordBatch batch;
    Expect(!batch.HasMarkEpoch(), "new batch must not expose an uninitialized mark epoch");
    batch.SetMarkEpoch(17);
    Expect(batch.HasMarkEpoch() && batch.GetMarkEpoch() == 17,
           "batch did not retain its mark epoch");

    batch.Add(1, 0, 1, [](const MSMarkWordBatchEntry&) {});
    batch.FlushAll([](const MSMarkWordBatchEntry&) {});
    Expect(batch.HasMarkEpoch() && batch.GetMarkEpoch() == 17,
           "flushing entries must not invalidate the batch-local cycle epoch");
}

void TestCachesBitmapPlaneBases()
{
    MSMarkWordBatch batch;
    uintptr_t allocBase = 0x100000;
    uintptr_t markBase = 0x200000;
    batch.SetBitmapPlaneBases(allocBase, markBase);
    Expect(batch.HasBitmapPlaneBases(), "batch did not retain bitmap plane bases");

    constexpr uint32_t regionIdx = 3;
    constexpr size_t wordIdx = 5;
    uintptr_t regionOffset =
        regionIdx * MapleRuntime::MSBitmapArena::kPlaneStrideBytes;
    Expect(
        reinterpret_cast<uintptr_t>(
            batch.GetAllocBitmap(regionIdx) + wordIdx) ==
            allocBase + regionOffset + wordIdx * sizeof(uint64_t),
        "batch alloc-plane address calculation mismatch");
    Expect(
        reinterpret_cast<uintptr_t>(
            batch.GetMarkBitmap(regionIdx) + wordIdx) ==
            markBase + regionOffset + wordIdx * sizeof(uint64_t),
        "batch mark-plane address calculation mismatch");
}

} // namespace

int main()
{
    TestCoalescesSameBitmapWord();
    TestEvictsOneEntryAtCapacity();
    TestCachesOneMarkEpochForBatchLifetime();
    TestCachesBitmapPlaneBases();
    return 0;
}
