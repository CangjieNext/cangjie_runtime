#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

#define MRT_USE_MARK_SWEEP 1
#define MRT_PANIC_H
#define MRT_BASE_LOG_H
#define MRT_MEM_UTILS_H

#define UNLIKELY(x) (x)
#define CHECK_DETAIL(x, ...)                                                                     \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::fprintf(stderr, "CHECK_DETAIL failed: %s:%d: %s\n", __FILE__, __LINE__, #x);   \
            std::abort();                                                                        \
        }                                                                                        \
    } while (false)
#define CHECK_E(x, ...)                                                                          \
    do {                                                                                         \
        if (x) {                                                                                 \
            std::fprintf(stderr, "CHECK_E failed: %s:%d: %s\n", __FILE__, __LINE__, #x);        \
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
#define LOG(...)
#define DLOG(...)

namespace MapleRuntime {
void MemorySet(uintptr_t dest, size_t, int c, size_t count)
{
    std::memset(reinterpret_cast<void*>(dest), c, count);
}
}

#define private public
#include "src/Heap/MAllocator/MSFreeManager.cpp"
#undef private

namespace {

using FreeManager = MapleRuntime::FreeManager;

void ExpectTrue(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void ExpectFalse(bool value, const char* message)
{
    if (value) {
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

void ExpectTake(FreeManager& manager, uint16_t homeShardId, uint32_t expectedIdx, const char* message)
{
    FreeManager::RegionIndex idx = 0;
    FreeManager::TryTakeResult result = manager.TryTakeRegions(1, homeShardId, idx);
    if (result != FreeManager::TryTakeResult::SUCCESS) {
        std::fprintf(stderr, "%s: expected SUCCESS, actual=%u\n", message, static_cast<unsigned>(result));
        std::abort();
    }
    ExpectEq(idx, expectedIdx, message);
}

void TestSingleRegionFastPathSeedsLocalHint()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment * 2;
    constexpr size_t kHeapBytes = static_cast<size_t>(kInitialRegions) * kRegionSize;

    void* heap = std::malloc(kHeapBytes);
    if (heap == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }

    FreeManager manager;
    manager.Initialize(
        kPageSize,
        kUnitsPerRegion,
        kInitialRegions,
        kInitialRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kSegmentSize,
        2);

    ExpectTake(manager, 0, 0, "shard0 first allocation");
    ExpectTrue(manager.shards_[0].fastTakeHintValid_, "shard0 fast hint should be set after local single-region allocation");
    ExpectEq(manager.shards_[0].fastTakeViewIdx_, 0, "shard0 fast hint should stay on first owned view");
    ExpectEq(manager.shards_[0].fastTakeWordOffset_, 0, "shard0 fast hint should point at first metadata word");
    ExpectFalse(manager.shards_[1].fastTakeHintValid_, "shard1 fast hint should stay untouched");

    std::free(heap);
}

void TestFastHintDoesNotPreemptReclaimSideOrdering()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr uint32_t kInitialRegions = 8;
    constexpr size_t kHeapBytes = static_cast<size_t>(kInitialRegions) * kRegionSize;

    void* heap = std::malloc(kHeapBytes);
    if (heap == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }

    FreeManager manager;
    manager.Initialize(
        kPageSize,
        kUnitsPerRegion,
        kInitialRegions,
        kInitialRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kHeapBytes,
        1);

    ExpectTake(manager, 0, 0, "first allocation");
    ExpectTake(manager, 0, 1, "second allocation");
    ExpectTake(manager, 0, 2, "third allocation");

    manager.ReclaimRegionsReleased(1, 1);

    ExpectTrue(manager.shards_[0].fastTakeHintValid_, "reclaim should not clear the existing fast hint");
    ExpectTake(manager, 0, 3, "take-side region should still win before reclaimed region");
    ExpectEq(manager.shards_[0].reclaimCursor_, 1, "reclaim cursor should keep reclaimed region parked");

    std::free(heap);
}

void TestMarkAllocatedOneLockedUpdatesSingleSegmentLocally()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr uint32_t kInitialRegions = 8;
    constexpr size_t kHeapBytes = static_cast<size_t>(kInitialRegions) * kRegionSize;

    void* heap = std::malloc(kHeapBytes);
    if (heap == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }

    FreeManager manager;
    manager.Initialize(
        kPageSize,
        kUnitsPerRegion,
        kInitialRegions,
        kInitialRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kHeapBytes,
        1);

    auto& shard = manager.shards_[0];
    auto* view = manager.FindOwnedSegmentViewByRegionLocked(shard, 0);
    if (view == nullptr) {
        std::fprintf(stderr, "expected owned segment view for region 0\n");
        std::abort();
    }

    bool reusedScavenged = false;
    manager.MarkAllocatedOneLocked(shard, *view, 0, reusedScavenged);

    ExpectFalse(reusedScavenged, "fresh region should not report reused scavenged");
    ExpectEq(shard.freeRegionCount_, kInitialRegions - 1, "single-bit fast path should decrement shard free count");
    ExpectEq(view->chunkSummaries_[0].max, kInitialRegions - 1, "single-bit fast path should refresh local chunk summary");

    std::free(heap);
}

void TestAllocationPrefersHotFreeBeforeScavengedFree()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr uint32_t kInitialRegions = 8;
    constexpr size_t kHeapBytes = static_cast<size_t>(kInitialRegions) * kRegionSize;

    void* heap = std::malloc(kHeapBytes);
    if (heap == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }

    FreeManager manager;
    manager.Initialize(
        kPageSize, kUnitsPerRegion, kInitialRegions, kInitialRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kHeapBytes, 1);

    {
        auto& shard = manager.shards_[0];
        std::lock_guard<std::mutex> lock(shard.shardMutex_);
        manager.MarkScavengingLocked(shard, 0, 1);
        manager.CompleteScavengingLocked(shard, 0, 1, true);
    }

    ExpectTake(manager, 0, 1,
               "allocator must skip a lower-address scavenged Region while hot-free exists");
    for (uint32_t expected = 2; expected < kInitialRegions; ++expected) {
        ExpectTake(manager, 0, expected,
                   "allocator must exhaust hot-free supply before cold reuse");
    }
    ExpectTake(manager, 0, 0,
               "allocator may reuse a scavenged Region after hot-free supply is exhausted");
    ExpectEq(manager.GetReusedScavengedRegionCount(), 1,
             "only the final cold fallback should count as scavenged reuse");

    std::free(heap);
}

void TestAllocationPrefersRemoteHotBeforeLocalScavenged()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr uint32_t kRegionsPerSegment = 4;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment * 2;
    constexpr size_t kHeapBytes = static_cast<size_t>(kInitialRegions) * kRegionSize;

    void* heap = std::malloc(kHeapBytes);
    if (heap == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }

    FreeManager manager;
    manager.Initialize(
        kPageSize, kUnitsPerRegion, kInitialRegions, kInitialRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kRegionsPerSegment * kRegionSize, 2);

    for (uint32_t expected = 0; expected < kRegionsPerSegment; ++expected) {
        ExpectTake(manager, 0, expected, "home shard setup allocation");
    }
    manager.ReclaimRegionsReleased(0, 1);
    {
        auto& shard = manager.shards_[0];
        std::lock_guard<std::mutex> lock(shard.shardMutex_);
        manager.MarkScavengingLocked(shard, 0, 1);
        manager.CompleteScavengingLocked(shard, 0, 1, true);
    }

    ExpectTake(manager, 0, kRegionsPerSegment,
               "remote hot-free must win before home-shard scavenged-free");
    ExpectEq(manager.GetReusedScavengedRegionCount(), 0,
             "global hot supply must avoid cold reuse");

    std::free(heap);
}

} // namespace

int main()
{
    TestSingleRegionFastPathSeedsLocalHint();
    TestFastHintDoesNotPreemptReclaimSideOrdering();
    TestMarkAllocatedOneLockedUpdatesSingleSegmentLocally();
    TestAllocationPrefersHotFreeBeforeScavengedFree();
    TestAllocationPrefersRemoteHotBeforeLocalScavenged();
    return 0;
}
