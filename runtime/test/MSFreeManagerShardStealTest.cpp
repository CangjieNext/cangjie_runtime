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

void TestSingleRegionStealBuildsBorrowedCache()
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
        ExpectTake(manager, 0, idx, "shard0 should drain its local segment before stealing");
    }

    ExpectTake(manager, 0, kRegionsPerSegment, "shard0 first remote steal should take donor's first region");
    ExpectTake(manager, 0, kRegionsPerSegment + 1, "shard0 should reuse second borrowed region before new steal");
    ExpectTake(manager, 0, kRegionsPerSegment + 2, "shard0 should reuse third borrowed region before new steal");
    ExpectTake(manager, 0, kRegionsPerSegment + 3, "shard0 should reuse fourth borrowed region before new steal");
    ExpectTake(manager, 1, kRegionsPerSegment + 4, "donor shard should skip borrowed batch and continue after it");

    std::free(heap);
}

} // namespace

int main()
{
    TestSingleRegionStealBuildsBorrowedCache();
    return 0;
}
