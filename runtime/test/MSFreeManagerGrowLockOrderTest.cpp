#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
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

    void OnCommittedHeapExtended(uintptr_t) override {}

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

class CountingGrowHandler final : public FreeManager::HeapRangeHandler {
public:
    void InitializeFreshRegions(FreeManager::RegionIndex, FreeManager::RegionCount) override
    {
        initializeCalls_.fetch_add(1, std::memory_order_relaxed);
    }

    void OnCommittedHeapExtended(uintptr_t) override
    {
        extendedCalls_.fetch_add(1, std::memory_order_relaxed);
    }

    size_t InitializeCalls() const
    {
        return initializeCalls_.load(std::memory_order_relaxed);
    }

    size_t ExtendedCalls() const
    {
        return extendedCalls_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<size_t> initializeCalls_ {0};
    std::atomic<size_t> extendedCalls_ {0};
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

void TestConcurrentMissesRecheckBeforeGrowing()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment;
    constexpr uint32_t kMaxRegions = kRegionsPerSegment * 3;
    constexpr size_t kHeapBytes = static_cast<size_t>(kMaxRegions) * kRegionSize;

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
        kMaxRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + static_cast<size_t>(kInitialRegions) * kRegionSize,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kSegmentSize,
        4);

    for (uint32_t i = 0; i < kInitialRegions; ++i) {
        FreeManager::RegionIndex idx = 0;
        ExpectTrue(manager.TryTakeRegions(1, 0, idx) == FreeManager::TryTakeResult::SUCCESS,
                   "initial segment should be drainable");
    }

    CountingGrowHandler handler;
    std::atomic<uint32_t> ready {0};
    std::atomic<bool> start {false};
    std::atomic<bool> firstResult {false};
    std::atomic<bool> secondResult {false};
    FreeManager::RegionIndex firstIdx = 0;
    FreeManager::RegionIndex secondIdx = 0;

    auto takeAfterMiss = [&](uint16_t shardId, FreeManager::RegionIndex& idx, std::atomic<bool>& result) {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        result.store(manager.TryTakeOrGrowRegions(1, shardId, idx, handler), std::memory_order_release);
    };

    std::thread first(takeAfterMiss, 1, std::ref(firstIdx), std::ref(firstResult));
    std::thread second(takeAfterMiss, 2, std::ref(secondIdx), std::ref(secondResult));
    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    ExpectTrue(firstResult.load(std::memory_order_acquire), "first concurrent miss should acquire a Region");
    ExpectTrue(secondResult.load(std::memory_order_acquire), "second concurrent miss should acquire a Region");
    ExpectTrue(firstIdx != secondIdx, "concurrent Grow acquisition must return unique Regions");
    ExpectTrue(manager.GetCommittedRegionCount() == kRegionsPerSegment * 2,
               "second miss should reuse the first Grow instead of growing another Segment");
    ExpectTrue(handler.InitializeCalls() == 1, "only one Segment should be initialized");
    ExpectTrue(handler.ExtendedCalls() == 1, "only one committed-end extension should be published");

    std::free(heap);
}

void TestReclaimedSupplyIsObservedBeforeGrowing()
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
        kPageSize,
        kUnitsPerRegion,
        kInitialRegions,
        kMaxRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + static_cast<size_t>(kInitialRegions) * kRegionSize,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kSegmentSize,
        4);

    FreeManager::RegionIndex reclaimedIdx = 0;
    for (uint32_t i = 0; i < kInitialRegions; ++i) {
        FreeManager::RegionIndex idx = 0;
        ExpectTrue(manager.TryTakeRegions(1, 0, idx) == FreeManager::TryTakeResult::SUCCESS,
                   "initial segment should be drainable before reclaim");
        reclaimedIdx = idx;
    }

    uint64_t generationBeforeReclaim = manager.GetAvailabilityGeneration();
    manager.ReclaimRegionsReleased(reclaimedIdx, 1);
    ExpectTrue(manager.GetAvailabilityGeneration() > generationBeforeReclaim,
               "reclaim must publish a new availability generation");

    CountingGrowHandler handler;
    FreeManager::RegionIndex resultIdx = 0;
    ExpectTrue(manager.TryTakeOrGrowRegions(1, 3, resultIdx, handler),
               "request should observe reclaimed supply");
    ExpectTrue(resultIdx == reclaimedIdx, "request should consume the reclaimed Region");
    ExpectTrue(manager.GetCommittedRegionCount() == kInitialRegions,
               "observed reclaimed supply must avoid growing another Segment");
    ExpectTrue(handler.InitializeCalls() == 0, "reclaim reuse must not initialize a Segment");
    ExpectTrue(handler.ExtendedCalls() == 0, "reclaim reuse must not publish a committed-end extension");

    std::free(heap);
}

} // namespace

int main()
{
    TestGrowWaitsOnPageLockBeforeTakingShardLock();
    TestGrowDoesNotHoldPageLockWhileWaitingForShardLock();
    TestConcurrentMissesRecheckBeforeGrowing();
    TestReclaimedSupplyIsObservedBeforeGrowing();
    return 0;
}
