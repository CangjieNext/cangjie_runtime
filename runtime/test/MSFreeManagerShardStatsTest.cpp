#include <chrono>
#include <cerrno>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sys/mman.h>
#include <thread>

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

namespace {
bool gForceReleaseFailure = false;
std::atomic<bool> gBlockFixedMmap { false };
std::atomic<bool> gFixedMmapEntered { false };
std::atomic<bool> gAllowFixedMmap { false };
std::mutex* gObservedShardMutex = nullptr;
bool gShardLockWasAvailableDuringRelease = false;

void* TestMmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (gForceReleaseFailure && (flags & MAP_FIXED) != 0) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (gBlockFixedMmap.load(std::memory_order_acquire) && (flags & MAP_FIXED) != 0) {
        gFixedMmapEntered.store(true, std::memory_order_release);
        while (!gAllowFixedMmap.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    return ::mmap(addr, length, prot, flags, fd, offset);
}
}

namespace MapleRuntime {
void MemorySet(uintptr_t dest, size_t, int c, size_t count)
{
    if (gObservedShardMutex != nullptr && gObservedShardMutex->try_lock()) {
        gShardLockWasAvailableDuringRelease = true;
        gObservedShardMutex->unlock();
    }
    std::memset(reinterpret_cast<void*>(dest), c, count);
}
}

#define mmap TestMmap
#define private public
#include "src/Heap/MAllocator/MSFreeManager.cpp"
#undef private
#undef mmap

namespace {

using FreeManager = MapleRuntime::FreeManager;

void* AllocateHeap(size_t bytes)
{
    void* heap = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap == MAP_FAILED) {
        std::fprintf(stderr, "mmap heap failed\n");
        std::abort();
    }
    return heap;
}

void FreeHeap(void* heap, size_t bytes)
{
    if (::munmap(heap, bytes) != 0) {
        std::fprintf(stderr, "munmap heap failed\n");
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

void ExpectHotFreeSnapshotsMatch(const FreeManager& manager, const char* message)
{
    ExpectEq(manager.GetHotFreeBytes(), manager.GetHotFreeBytesReference(), message);
}

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

void TestPerShardCountsTrackOwnerAndBorrowedBatchConsumption()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment * 2;
    constexpr size_t kHeapBytes = static_cast<size_t>(kInitialRegions) * kRegionSize;

    void* heap = AllocateHeap(kHeapBytes);

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

    ExpectHotFreeSnapshotsMatch(manager, "initial aggregate hot-free snapshot");

    ExpectEq(manager.GetOwnedFreeRegionCount(0), kRegionsPerSegment, "owner0 initial free count");
    ExpectEq(manager.GetOwnedFreeRegionCount(1), kRegionsPerSegment, "owner1 initial free count");
    ExpectEq(manager.GetHotFreeRegionCount(0), kRegionsPerSegment, "owner0 initial hot-free count");
    ExpectEq(manager.GetHotFreeRegionCount(1), kRegionsPerSegment, "owner1 initial hot-free count");

    for (uint32_t idx = 0; idx < kRegionsPerSegment; ++idx) {
        ExpectTake(manager, 1, 0, idx, "shard0 should drain local segment");
    }
    ExpectEq(manager.GetOwnedFreeRegionCount(0), 0, "owner0 free count after local drain");
    ExpectEq(manager.GetHotFreeRegionCount(0), 0, "owner0 hot-free count after local drain");
    ExpectHotFreeSnapshotsMatch(manager, "aggregate after local drain");

    ExpectTake(manager, 1, 0, kRegionsPerSegment, "shard0 first steal should take donor region");
    ExpectEq(manager.GetOwnedFreeRegionCount(1), kRegionsPerSegment - 4, "owner1 free count after batch steal");
    ExpectEq(manager.GetHotFreeRegionCount(1), kRegionsPerSegment - 4, "owner1 hot-free count after batch steal");
    ExpectHotFreeSnapshotsMatch(manager, "aggregate after batch steal");

    ExpectTake(manager, 1, 0, kRegionsPerSegment + 1, "shard0 should consume first borrowed region");
    ExpectTake(manager, 1, 0, kRegionsPerSegment + 2, "shard0 should consume second borrowed region");
    ExpectTake(manager, 1, 0, kRegionsPerSegment + 3, "shard0 should consume third borrowed region");
    ExpectEq(manager.GetOwnedFreeRegionCount(1), kRegionsPerSegment - 4,
             "consuming borrowed batch must not charge donor twice");
    ExpectTake(manager, 1, 0, kRegionsPerSegment + 4,
               "shard0 should steal a new donor batch after borrowed batch is exhausted");
    ExpectEq(manager.GetOwnedFreeRegionCount(1), kRegionsPerSegment - 8,
             "new steal should charge the next donor batch");

    FreeHeap(heap, kHeapBytes);
}

void TestScavengeRotatesAcrossOwnerShards()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment * 2;
    constexpr size_t kHeapBytes = static_cast<size_t>(kInitialRegions) * kRegionSize;

    void* heap = AllocateHeap(kHeapBytes);

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

    ExpectHotFreeSnapshotsMatch(manager, "scavenge initial aggregate");

    ExpectEq(manager.ScavengeFreeRegionsUpTo(kRegionSize), kRegionSize, "first scavenge slice");
    ExpectEq(manager.GetScavengedRegionCount(0), 1, "owner0 should be scavenged first");
    ExpectEq(manager.GetScavengedRegionCount(1), 0, "owner1 should remain untouched after first slice");
    ExpectHotFreeSnapshotsMatch(manager, "aggregate after first scavenge slice");

    ExpectEq(manager.ScavengeFreeRegionsUpTo(kRegionSize), kRegionSize, "second scavenge slice");
    ExpectEq(manager.GetScavengedRegionCount(0), 1, "owner0 scavenged count after second slice");
    ExpectEq(manager.GetScavengedRegionCount(1), 1, "owner1 should be scavenged second");
    ExpectHotFreeSnapshotsMatch(manager, "aggregate after second scavenge slice");

    FreeHeap(heap, kHeapBytes);
}

void TestScavengeSkipsBusyShardWithTryLock()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment * 2;
    constexpr size_t kHeapBytes = static_cast<size_t>(kInitialRegions) * kRegionSize;

    void* heap = AllocateHeap(kHeapBytes);

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

    size_t releasedBytes = 0;
    std::unique_lock<std::mutex> holdShard0(manager.shards_[0].shardMutex_);
    std::thread worker([&]() {
        releasedBytes = manager.ScavengeFreeRegionsUpTo(kRegionSize);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    holdShard0.unlock();
    worker.join();

    ExpectEq(releasedBytes, kRegionSize, "try-lock scavenge should still release one region");
    ExpectEq(manager.GetScavengedRegionCount(0), 0, "busy shard0 should be skipped");
    ExpectEq(manager.GetScavengedRegionCount(1), 1, "shard1 should receive the scavenge budget after shard0 skip");
    ExpectHotFreeSnapshotsMatch(manager, "aggregate after busy-shard scavenge");

    FreeHeap(heap, kHeapBytes);
}

void TestScavengeReleasesOutsideShardLock()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr size_t kHeapBytes = static_cast<size_t>(kRegionsPerSegment) * kRegionSize;

    void* heap = AllocateHeap(kHeapBytes);
    FreeManager manager;
    manager.Initialize(kPageSize, kUnitsPerRegion, kRegionsPerSegment, kRegionsPerSegment,
                       reinterpret_cast<uintptr_t>(heap),
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       kSegmentSize, 1);

    gObservedShardMutex = &manager.shards_[0].shardMutex_;
    gShardLockWasAvailableDuringRelease = false;
    ExpectEq(manager.ScavengeFreeRegionsUpTo(kRegionSize), kRegionSize,
             "outside-lock scavenge should release one region");
    gObservedShardMutex = nullptr;
    if (!gShardLockWasAvailableDuringRelease) {
        std::fprintf(stderr, "ReleaseUnits must run without shardMutex_ held\n");
        std::abort();
    }

    FreeHeap(heap, kHeapBytes);
}

void TestScavengeFailureRollsBackToHotFree()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr size_t kHeapBytes = static_cast<size_t>(kRegionsPerSegment) * kRegionSize;

    void* heap = AllocateHeap(kHeapBytes);
    FreeManager manager;
    manager.Initialize(kPageSize, kUnitsPerRegion, kRegionsPerSegment, kRegionsPerSegment,
                       reinterpret_cast<uintptr_t>(heap),
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       kSegmentSize, 1);

    size_t hotFreeBefore = manager.GetHotFreeBytes();
    gForceReleaseFailure = true;
    ExpectEq(manager.ScavengeFreeRegionsUpTo(kRegionSize), 0,
             "failed OS release must not be reported as scavenged");
    gForceReleaseFailure = false;
    ExpectEq(manager.GetScavengedRegionCount(0), 0,
             "failed OS release must not publish scavenged metadata");
    ExpectEq(manager.GetHotFreeBytes(), hotFreeBefore,
             "failed OS release must restore hot-free accounting");

    FreeManager::RegionIndex idx = 0;
    if (manager.TryTakeRegions(1, 0, idx) != FreeManager::TryTakeResult::SUCCESS) {
        std::fprintf(stderr, "rolled-back Region must remain allocatable\n");
        std::abort();
    }

    FreeHeap(heap, kHeapBytes);
}

void TestWholeSegmentReleaseUsesTwoPhasePublication()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr size_t kHeapBytes = kSegmentSize;

    void* heap = AllocateHeap(kHeapBytes);
    FreeManager manager;
    manager.Initialize(kPageSize, kUnitsPerRegion, kRegionsPerSegment, kRegionsPerSegment,
                       reinterpret_cast<uintptr_t>(heap),
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       kSegmentSize, 1);

    gForceReleaseFailure = true;
    ExpectEq(manager.ReleaseCompletelyFreeSegment(0), 0,
             "failed whole-segment release must report zero bytes");
    gForceReleaseFailure = false;
    ExpectEq(manager.GetScavengedRegionCount(0), 0,
             "failed whole-segment release must roll back metadata");
    ExpectEq(manager.GetHotFreeBytes(), kHeapBytes,
             "failed whole-segment release must restore all hot-free Regions");

    gObservedShardMutex = &manager.shards_[0].shardMutex_;
    gShardLockWasAvailableDuringRelease = false;
    ExpectEq(manager.ReleaseCompletelyFreeSegment(0), kHeapBytes,
             "whole-segment release should publish all released Regions");
    gObservedShardMutex = nullptr;
    if (!gShardLockWasAvailableDuringRelease) {
        std::fprintf(stderr, "whole-segment ReleaseUnits must run without shardMutex_ held\n");
        std::abort();
    }

    FreeHeap(heap, kHeapBytes);
}

void TestShrinkKeepsTopologyWhenReleaseFails()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment * 2;
    constexpr size_t kHeapBytes = kSegmentSize * 2;

    void* heap = AllocateHeap(kHeapBytes);
    FreeManager manager;
    manager.Initialize(kPageSize, kUnitsPerRegion, kInitialRegions, kInitialRegions,
                       reinterpret_cast<uintptr_t>(heap),
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       kSegmentSize, 1);

    gForceReleaseFailure = true;
    manager.ShrinkHeap();
    gForceReleaseFailure = false;
    ExpectEq(manager.GetCommittedRegionCount(), kInitialRegions,
             "failed tail release must not shrink committed topology");
    ExpectEq(manager.GetOwnedSegmentCount(0), 2,
             "failed tail release must not detach segment metadata");

    manager.ShrinkHeap();
    ExpectEq(manager.GetCommittedRegionCount(), kRegionsPerSegment,
             "successful tail release should shrink one segment");
    ExpectEq(manager.GetOwnedSegmentCount(0), 1,
             "successful tail release should detach one segment view");

    FreeHeap(heap, kHeapBytes);
}

void TestClaimedRegionCannotBeAllocatedDuringRelease()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr size_t kHeapBytes = kSegmentSize;

    void* heap = AllocateHeap(kHeapBytes);
    FreeManager manager;
    manager.Initialize(kPageSize, kUnitsPerRegion, kRegionsPerSegment, kRegionsPerSegment,
                       reinterpret_cast<uintptr_t>(heap),
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       kSegmentSize, 1);
    ExpectTake(manager, kRegionsPerSegment - 1, 0, 0,
               "setup should leave only the highest Region hot-free");

    gFixedMmapEntered.store(false, std::memory_order_relaxed);
    gAllowFixedMmap.store(false, std::memory_order_relaxed);
    gBlockFixedMmap.store(true, std::memory_order_release);
    size_t releasedBytes = 0;
    std::thread worker([&]() {
        releasedBytes = manager.ScavengeFreeRegionsUpTo(kRegionSize);
    });
    while (!gFixedMmapEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    ExpectEq(manager.shards_[0].scavengingRegionCount_, 1,
             "claimed Region should be tracked as scavenging");
    FreeManager::RegionIndex idx = 0;
    if (manager.TryTakeRegions(1, 0, idx) != FreeManager::TryTakeResult::EMPTY) {
        std::fprintf(stderr, "allocator must not take a Region while its OS release is in progress\n");
        std::abort();
    }

    gAllowFixedMmap.store(true, std::memory_order_release);
    worker.join();
    gBlockFixedMmap.store(false, std::memory_order_release);
    ExpectEq(releasedBytes, kRegionSize, "blocked release should complete successfully");
    ExpectEq(manager.shards_[0].scavengingRegionCount_, 0,
             "completed release should clear scavenging accounting");

    FreeHeap(heap, kHeapBytes);
}

} // namespace

int main()
{
    TestPerShardCountsTrackOwnerAndBorrowedBatchConsumption();
    TestScavengeRotatesAcrossOwnerShards();
    TestScavengeSkipsBusyShardWithTryLock();
    TestScavengeReleasesOutsideShardLock();
    TestScavengeFailureRollsBackToHotFree();
    TestWholeSegmentReleaseUsesTwoPhasePublication();
    TestShrinkKeepsTopologyWhenReleaseFails();
    TestClaimedRegionCannotBeAllocatedDuringRelease();
    return 0;
}
