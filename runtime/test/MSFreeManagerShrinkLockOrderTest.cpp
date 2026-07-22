#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
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

void InitializeManager(FreeManager& manager, void*& heap)
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment;
    constexpr size_t kHeapBytes = static_cast<size_t>(kInitialRegions) * kRegionSize;

    heap = std::malloc(kHeapBytes);
    if (heap == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }

    manager.Initialize(
        kPageSize,
        kUnitsPerRegion,
        kInitialRegions,
        kInitialRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kSegmentSize,
        1);
}

void TestSegmentSnapshotDoesNotHoldPageLockWhileWaitingForShard()
{
    void* heap = nullptr;
    FreeManager manager;
    InitializeManager(manager, heap);

    manager.shards_[0].shardMutex_.lock();
    std::atomic<bool> started { false };
    std::thread worker([&]() {
        started.store(true, std::memory_order_release);
        (void)manager.GetSegmentFreeSnapshot(0);
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    bool pageLocked = manager.pageHeapMutex_.try_lock();
    ExpectTrue(pageLocked,
               "GetSegmentFreeSnapshot must not hold pageHeapMutex_ while waiting for shardMutex_");
    if (pageLocked) {
        manager.pageHeapMutex_.unlock();
    }

    manager.shards_[0].shardMutex_.unlock();
    worker.join();
    std::free(heap);
}

void TestReleaseFreeSegmentDoesNotHoldPageLockWhileWaitingForShard()
{
    void* heap = nullptr;
    FreeManager manager;
    InitializeManager(manager, heap);

    manager.shards_[0].shardMutex_.lock();
    std::atomic<bool> started { false };
    std::thread worker([&]() {
        started.store(true, std::memory_order_release);
        (void)manager.ReleaseCompletelyFreeSegment(0);
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    bool pageLocked = manager.pageHeapMutex_.try_lock();
    ExpectTrue(pageLocked,
               "ReleaseCompletelyFreeSegment must not hold pageHeapMutex_ while waiting for shardMutex_");
    if (pageLocked) {
        manager.pageHeapMutex_.unlock();
    }

    manager.shards_[0].shardMutex_.unlock();
    worker.join();
    std::free(heap);
}

} // namespace

int main()
{
    TestSegmentSnapshotDoesNotHoldPageLockWhileWaitingForShard();
    TestReleaseFreeSegmentDoesNotHoldPageLockWhileWaitingForShard();
    return 0;
}
