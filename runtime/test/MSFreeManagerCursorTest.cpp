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

void ExpectEq(uint32_t actual, uint32_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n", message, expected, actual);
        std::abort();
    }
}

void ExpectTake(FreeManager& manager, uint32_t expectedIdx, const char* message)
{
    FreeManager::RegionIndex idx = 0;
    FreeManager::TryTakeResult result = manager.TryTakeRegions(1, MapleRuntime::kMSInvalidShardId, idx);
    if (result != FreeManager::TryTakeResult::SUCCESS) {
        std::fprintf(stderr, "%s: expected SUCCESS, actual=%u\n", message, static_cast<unsigned>(result));
        std::abort();
    }
    ExpectEq(idx, expectedIdx, message);
}

void TestRecentlyFreedRegionStaysOnReclaimSide()
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
        kInitialRegions * kUnitsPerRegion,
        kPageSize,
        kUnitsPerRegion,
        kInitialRegions,
        kInitialRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kHeapBytes,
        1);

    ExpectTake(manager, 0, "first allocation");
    ExpectTake(manager, 1, "second allocation");
    ExpectTake(manager, 2, "third allocation");

    manager.ReclaimRegionsReleased(1, 1);

    ExpectTake(manager, 3, "fresh take should stay on take-side before reusing reclaimed region");
    ExpectTake(manager, 4, "fourth fresh region");
    ExpectTake(manager, 5, "fifth fresh region");
    ExpectTake(manager, 6, "sixth fresh region");
    ExpectTake(manager, 7, "seventh fresh region");
    ExpectTake(manager, 1, "allocator should return to reclaim-side after take-side exhausts");

    std::free(heap);
}

} // namespace

int main()
{
    TestRecentlyFreedRegionStaysOnReclaimSide();
    return 0;
}
