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
using MapleRuntime::MSPendingMarkRegionBatch;
using MapleRuntime::MSRegionInfo;

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

    uint64_t observedMask = 0;
    batch.FlushAll([&](const MSMarkWordBatchEntry& entry) {
        ++flushes;
        Expect(entry.RegionIndex() == 7 && entry.WordIndex() == 3,
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
        flushed.push_back(entry.RegionIndex());
    };

    for (size_t i = 0; i < MSMarkWordBatch::kCapacity; ++i) {
        batch.Add(static_cast<uint32_t>(i), 0, 1, flush);
    }
    Expect(flushed.empty(), "a non-full batch must not evict");
    batch.Add(static_cast<uint32_t>(MSMarkWordBatch::kCapacity), 0, 2, flush);
    Expect(flushed.size() == 1,
           "full batch must evict exactly one set-local victim");

    batch.FlushAll(flush);
    Expect(flushed.size() == MSMarkWordBatch::kCapacity + 1,
           "FlushAll did not flush every resident entry");
}

void TestSparseBatchReuseVisitsOnlyCurrentResidents()
{
    MSMarkWordBatch batch;
    batch.Add(3, 1, 1, [](const MSMarkWordBatchEntry&) {});
    batch.FlushAll([](const MSMarkWordBatchEntry&) {});

    batch.Add(9, 2, 4, [](const MSMarkWordBatchEntry&) {});
    size_t visits = 0;
    batch.FlushAll([&](const MSMarkWordBatchEntry& entry) {
        ++visits;
        Expect(entry.RegionIndex() == 9 && entry.WordIndex() == 2 && entry.mask == 4,
               "sparse batch reuse exposed a stale entry");
    });
    Expect(visits == 1 && batch.IsEmpty(),
           "sparse FlushAll must visit only current residents");
}

void TestSharedBatchCapacity()
{
    Expect(MSMarkWordBatch::kCapacity == 32,
           "all GC-owned batches must retain 32 bitmap-word entries");
    Expect(MSMarkWordBatch::kSetCount == 8 && MSMarkWordBatch::kWays == 4,
           "GC-owned batch must use eight four-way cache-line sets");
}

void TestFourWaySetDoesNotEvictEarly()
{
    MSMarkWordBatch batch;
    size_t flushes = 0;
    auto flush = [&flushes](const MSMarkWordBatchEntry&) {
        ++flushes;
    };

    // With wordIdx=0 these Region indices map to the same power-of-two set.
    // Four-way storage must retain all four before evicting one for the fifth.
    batch.Add(0, 0, 1, flush);
    batch.Add(16, 0, 1, flush);
    batch.Add(32, 0, 1, flush);
    batch.Add(48, 0, 1, flush);
    Expect(flushes == 0,
           "four entries in one set must not trigger an early flush");
    batch.Add(64, 0, 1, flush);
    Expect(flushes == 1,
           "the fifth entry in one set must replace exactly one resident");
    batch.FlushAll(flush);
    Expect(flushes == 5 && batch.IsEmpty(),
           "four resident entries must remain after one set-local eviction");
}

void TestCachesSharedBitmapContextForBatchLifetime()
{
    uintptr_t allocBase = 0x100000;
    uintptr_t markBase = 0x200000;
    MapleRuntime::MSMarkBitmapContext context {17, allocBase, markBase};
    MSMarkWordBatch batch;
    batch.SetBitmapContext(context);
    Expect(batch.GetMarkEpoch() == context.markEpoch,
           "batch did not retain its mark epoch");

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

    batch.Add(1, 0, 1, [](const MSMarkWordBatchEntry&) {});
    batch.FlushAll([](const MSMarkWordBatchEntry&) {});
    Expect(batch.GetMarkEpoch() == context.markEpoch,
           "flushing entries must not invalidate the batch-local context");
}

void TestInitializesBatchFromSharedBitmapContext()
{
    MSMarkWordBatch batch;
    MapleRuntime::MSMarkBitmapContext context {
        17, 0x100000, 0x200000};
    batch.SetBitmapContext(context);

    Expect(batch.GetMarkEpoch() == context.markEpoch,
           "batch did not retain the shared mark epoch");
    Expect(reinterpret_cast<uintptr_t>(batch.GetAllocBitmap(3)) ==
               context.allocPlaneBase +
                   3 * MapleRuntime::MSBitmapArena::kPlaneStrideBytes,
           "batch alloc bitmap did not use the shared context");
    Expect(reinterpret_cast<uintptr_t>(batch.GetMarkBitmap(3)) ==
               context.markPlaneBase +
                   3 * MapleRuntime::MSBitmapArena::kPlaneStrideBytes,
           "batch mark bitmap did not use the shared context");
}

void TestPendingRegionBatchMergesBitmapWords()
{
    MSRegionInfo first {};
    MSRegionInfo second {};
    MSPendingMarkRegionBatch<2> pending;

    Expect(pending.TryAdd(&first, 3),
           "first pending Region must fit");
    Expect(pending.TryAdd(&first, 9),
           "another word in the same Region must merge");
    Expect(pending.TryAdd(&second, 5),
           "second pending Region must fit");

    size_t publications = 0;
    pending.FlushAll([&](MSRegionInfo* info, uint64_t wordMask) {
        ++publications;
        if (info == &first) {
            Expect(wordMask == ((UINT64_C(1) << 3) | (UINT64_C(1) << 9)),
                   "first Region lost a pending bitmap word");
        } else {
            Expect(info == &second && wordMask == (UINT64_C(1) << 5),
                   "second Region publication changed");
        }
    });
    Expect(publications == 2 && pending.IsEmpty(),
           "pending Region FlushAll must publish and clear every Region");
}

void TestPendingRegionBatchReportsCapacityWithoutMutation()
{
    MSRegionInfo first {};
    MSRegionInfo second {};
    MSPendingMarkRegionBatch<1> pending;

    Expect(pending.TryAdd(&first, 1), "first pending Region must fit");
    Expect(!pending.TryAdd(&second, 2),
           "a distinct Region beyond capacity must request a drain");

    MSRegionInfo* observed = nullptr;
    uint64_t observedMask = 0;
    pending.FlushAll([&](MSRegionInfo* info, uint64_t wordMask) {
        observed = info;
        observedMask = wordMask;
    });
    Expect(observed == &first && observedMask == (UINT64_C(1) << 1),
           "capacity failure corrupted the resident publication");
}

} // namespace

int main()
{
    TestCoalescesSameBitmapWord();
    TestSharedBatchCapacity();
    TestFourWaySetDoesNotEvictEarly();
    TestEvictsOneEntryAtCapacity();
    TestSparseBatchReuseVisitsOnlyCurrentResidents();
    TestCachesSharedBitmapContextForBatchLifetime();
    TestInitializesBatchFromSharedBitmapContext();
    TestPendingRegionBatchMergesBitmapWords();
    TestPendingRegionBatchReportsCapacityWithoutMutation();
    return 0;
}
