#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
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

#include "src/Heap/MAllocator/MSFreeManager.cpp"

namespace {

using FreeManager = MapleRuntime::FreeManager;

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
    if (idx != expectedIdx) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n", message, expectedIdx, idx);
        std::abort();
    }
}

void TestBatchReclaimRoutesMixedOwnersBackTogether()
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
    ExpectTake(manager, 1, kRegionsPerSegment, "shard1 first allocation");
    ExpectEq(manager.GetOwnedFreeRegionCount(0), kRegionsPerSegment - 1, "owner0 free count after alloc");
    ExpectEq(manager.GetOwnedFreeRegionCount(1), kRegionsPerSegment - 1, "owner1 free count after alloc");

    std::vector<FreeManager::RegionSpan> spans {
        { 0, 1 },
        { kRegionsPerSegment, 1 },
    };
    manager.ReclaimRegionsReleasedBatch(spans);

    ExpectEq(manager.GetOwnedFreeRegionCount(0), kRegionsPerSegment, "owner0 free count after batch reclaim");
    ExpectEq(manager.GetOwnedFreeRegionCount(1), kRegionsPerSegment, "owner1 free count after batch reclaim");

    std::free(heap);
}

void TestBatchReclaimSplitsSpanAcrossOwnerShards()
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

    for (uint32_t idx = 0; idx < kRegionsPerSegment; ++idx) {
        ExpectTake(manager, 0, idx, "shard0 should drain local segment up to boundary");
    }
    ExpectTake(manager, 1, kRegionsPerSegment, "shard1 should take the first region in its owned segment");

    ExpectEq(manager.GetOwnedFreeRegionCount(0), 0, "owner0 free count after draining boundary segment");
    ExpectEq(manager.GetOwnedFreeRegionCount(1), kRegionsPerSegment - 1, "owner1 free count after taking boundary+1 region");

    std::vector<FreeManager::RegionSpan> spans {
        { static_cast<FreeManager::RegionIndex>(kRegionsPerSegment - 1), 2 },
    };
    manager.ReclaimRegionsReleasedBatch(spans);

    ExpectEq(manager.GetOwnedFreeRegionCount(0), 1, "owner0 should recover the tail region on its side of the boundary");
    ExpectEq(manager.GetOwnedFreeRegionCount(1), kRegionsPerSegment, "owner1 should recover the head region on its side of the boundary");

    std::free(heap);
}

} // namespace

int main()
{
    TestBatchReclaimRoutesMixedOwnersBackTogether();
    TestBatchReclaimSplitsSpanAcrossOwnerShards();
    return 0;
}
