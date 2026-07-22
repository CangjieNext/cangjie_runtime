#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

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

void ExpectTake(FreeManager& manager, uint32_t num, uint16_t homeShardId, uint32_t expectedIdx, const char* message)
{
    FreeManager::RegionIndex idx = 0;
    FreeManager::TryTakeResult result = manager.TryTakeRegions(num, homeShardId, idx);
    if (result != FreeManager::TryTakeResult::SUCCESS) {
        std::fprintf(stderr, "%s: expected SUCCESS, actual=%u\n", message, static_cast<unsigned>(result));
        std::abort();
    }
    if (idx != expectedIdx) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n", message, expectedIdx, idx);
        std::abort();
    }
}

void TestContiguousRunStealsFromDonorOwnedSegment()
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
        kInitialRegions * kUnitsPerRegion,
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
        ExpectTake(manager, 1, 0, idx, "shard0 should drain its local segment before large steal");
    }

    ExpectTake(manager, 4, 0, kRegionsPerSegment, "shard0 should steal a contiguous donor run for large request");
    ExpectTake(manager, 1, 1, kRegionsPerSegment + 4, "donor shard should continue after stolen large run");

    std::free(heap);
}

void TestLargeRunLargerThanChunkFitsInSingleFreeSegment()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment;
    constexpr size_t kHeapBytes = static_cast<size_t>(kInitialRegions) * kRegionSize;
    constexpr uint32_t kLargeRun = 128;

    void* heap = std::malloc(kHeapBytes);
    if (heap == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }

    FreeManager manager;
    manager.Initialize(
        kInitialRegions * kUnitsPerRegion,
        kPageSize,
        kUnitsPerRegion,
        kInitialRegions,
        kInitialRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kSegmentSize,
        1);

    ExpectTake(manager, kLargeRun, 0, 0,
               "a cross-chunk large run should be found inside a fully free owned segment");

    std::free(heap);
}

void TestLargeStealPrefersDonorWithLargestFreeRun()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment * 3;
    constexpr size_t kHeapBytes = static_cast<size_t>(kInitialRegions) * kRegionSize;

    void* heap = std::malloc(kHeapBytes);
    if (heap == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }

    FreeManager manager;
    manager.Initialize(
        kInitialRegions * kUnitsPerRegion,
        kPageSize,
        kUnitsPerRegion,
        kInitialRegions,
        kInitialRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kSegmentSize,
        3);

    for (uint32_t idx = 0; idx < kRegionsPerSegment; ++idx) {
        ExpectTake(manager, 1, 0, idx, "shard0 should drain its local segment before large steal preference check");
    }
    for (uint32_t idx = 0; idx < kRegionsPerSegment - 4; ++idx) {
        ExpectTake(manager, 1, 1, kRegionsPerSegment + idx, "shard1 should be shaped into a smaller tail run");
    }

    ExpectTake(manager, 4, 0, kRegionsPerSegment * 2,
               "large steal should prefer the donor shard with the largest free run");

    std::free(heap);
}

} // namespace

int main()
{
    TestContiguousRunStealsFromDonorOwnedSegment();
    TestLargeRunLargerThanChunkFitsInSingleFreeSegment();
    TestLargeStealPrefersDonorWithLargestFreeRun();
    return 0;
}
