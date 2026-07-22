#include <atomic>
#include <chrono>
#include <condition_variable>
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

class BlockingGrowHandler final : public FreeManager::HeapRangeHandler {
public:
    void InitializeFreshRegions(FreeManager::RegionIndex, FreeManager::RegionCount) override
    {
        std::unique_lock<std::mutex> lock(mu_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this]() { return proceed_; });
    }

    void OnCommittedHeapChanged(uintptr_t) override {}

    void WaitUntilEntered()
    {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this]() { return entered_; });
    }

    void Release()
    {
        std::lock_guard<std::mutex> lock(mu_);
        proceed_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool proceed_ = false;
};

void TestGrowWaitsOnPageLockBeforeTakingShardLock()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment;
    constexpr uint32_t kMaxRegions = kRegionsPerSegment * 2;
    constexpr size_t kHeapBytes = static_cast<size_t>(kMaxRegions) * kRegionSize;

    void* heap = std::malloc(kHeapBytes);
    if (heap == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }

    FreeManager manager;
    manager.Initialize(
        kMaxRegions * kUnitsPerRegion,
        kPageSize,
        kUnitsPerRegion,
        kInitialRegions,
        kMaxRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + static_cast<size_t>(kInitialRegions) * kRegionSize,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kSegmentSize,
        4);

    BlockingGrowHandler handler;
    std::atomic<bool> growResult { false };
    std::unique_lock<std::mutex> pageLock(manager.pageHeapMutex_);
    std::thread growThread([&]() { growResult.store(manager.GrowOneSegment(3, handler), std::memory_order_relaxed); });

    handler.WaitUntilEntered();
    handler.Release();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    bool shardLocked = manager.shards_[3].shardMutex_.try_lock();
    ExpectTrue(shardLocked, "GrowOneSegment must block on pageHeapMutex_ before acquiring shardMutex_");
    if (shardLocked) {
        manager.shards_[3].shardMutex_.unlock();
    }

    pageLock.unlock();
    growThread.join();
    ExpectTrue(growResult.load(std::memory_order_relaxed), "GrowOneSegment should succeed after releasing page lock");

    std::free(heap);
}

void TestGrowDoesNotHoldPageLockWhileWaitingForShardLock()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment;
    constexpr uint32_t kMaxRegions = kRegionsPerSegment * 2;
    constexpr size_t kHeapBytes = static_cast<size_t>(kMaxRegions) * kRegionSize;

    void* heap = std::malloc(kHeapBytes);
    if (heap == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }

    FreeManager manager;
    manager.Initialize(
        kMaxRegions * kUnitsPerRegion,
        kPageSize,
        kUnitsPerRegion,
        kInitialRegions,
        kMaxRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + static_cast<size_t>(kInitialRegions) * kRegionSize,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kSegmentSize,
        4);

    BlockingGrowHandler handler;
    std::atomic<bool> growResult { false };
    manager.shards_[3].shardMutex_.lock();
    std::thread growThread([&]() { growResult.store(manager.GrowOneSegment(3, handler), std::memory_order_relaxed); });

    handler.WaitUntilEntered();
    handler.Release();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    bool pageLocked = manager.pageHeapMutex_.try_lock();
    ExpectTrue(pageLocked, "GrowOneSegment must not hold pageHeapMutex_ while waiting for shardMutex_");
    if (pageLocked) {
        manager.pageHeapMutex_.unlock();
    }

    manager.shards_[3].shardMutex_.unlock();
    growThread.join();
    ExpectTrue(growResult.load(std::memory_order_relaxed), "GrowOneSegment should succeed after releasing shard lock");

    std::free(heap);
}

} // namespace

int main()
{
    TestGrowWaitsOnPageLockBeforeTakingShardLock();
    TestGrowDoesNotHoldPageLockWhileWaitingForShardLock();
    return 0;
}
