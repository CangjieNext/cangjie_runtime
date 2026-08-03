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
std::atomic<size_t> gMemorySetCalls { 0 };

void* TestMmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    if ((flags & MAP_FIXED) != 0 && gObservedShardMutex != nullptr &&
        gObservedShardMutex->try_lock()) {
        gShardLockWasAvailableDuringRelease = true;
        gObservedShardMutex->unlock();
    }
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
    gMemorySetCalls.fetch_add(1, std::memory_order_relaxed);
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

uint64_t ValidRegionMask(size_t wordIdx, size_t validRegions)
{
    size_t wordStart = wordIdx * FreeManager::kChunkBits;
    if (wordStart >= validRegions) {
        return 0;
    }
    size_t validBits = std::min(FreeManager::kChunkBits, validRegions - wordStart);
    return validBits == FreeManager::kChunkBits ? ~0ULL : ((1ULL << validBits) - 1);
}

void ExpectHotWordSummaryMatches(const FreeManager::OwnedSegmentView& view,
                                 size_t committedLocal,
                                 const char* message)
{
    uint64_t expected = 0;
    for (size_t wordIdx = 0; wordIdx < view.bitmapWordCount_; ++wordIdx) {
        uint64_t hotFree = (~view.allocBits_[wordIdx]) & (~view.scavengedBits_[wordIdx]) &
                           ValidRegionMask(wordIdx, committedLocal);
        if (hotFree != 0) {
            expected |= 1ULL << wordIdx;
        }
    }
    ExpectEq(view.hotWordMask_, expected, message);
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

void TestHotWordSummaryInitializationAndTransitions()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr uint32_t kRegionCount = 70;
    constexpr size_t kHeapBytes = static_cast<size_t>(kRegionCount) * kRegionSize;

    void* heap = AllocateHeap(kHeapBytes);
    FreeManager manager;
    manager.Initialize(
        kPageSize,
        kUnitsPerRegion,
        kRegionCount,
        kRegionCount,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kHeapBytes,
        1);

    FreeManager::FreeManagerShard& shard = manager.GetShard(0);
    std::lock_guard<std::mutex> lock(shard.shardMutex_);
    FreeManager::OwnedSegmentView& view = shard.ownedViews_.front();
    ExpectHotWordSummaryMatches(view, kRegionCount, "fresh view hot-word summary");
    ExpectEq(view.hotWordMask_, 0x3, "70 valid Regions require exactly two hot words");

    bool reusedScavenged = false;
    manager.MarkAllocatedLocked(shard, 0, 64, reusedScavenged);
    ExpectHotWordSummaryMatches(view, kRegionCount, "summary after first word allocation");
    ExpectEq(view.hotWordMask_, 0x2, "allocating first word leaves only tail word hot");

    manager.MarkFreeLocked(shard, 0, 64);
    ExpectHotWordSummaryMatches(view, kRegionCount, "summary after reclaim");
    ExpectEq(view.hotWordMask_, 0x3, "reclaim restores first hot word");

    manager.MarkUnavailableLocked(shard, 64, 6);
    ExpectHotWordSummaryMatches(view, kRegionCount, "summary after tail becomes unavailable");
    ExpectEq(view.hotWordMask_, 0x1, "unavailable tail must not appear hot");

    manager.MarkScavengingLocked(shard, 0, 64);
    ExpectHotWordSummaryMatches(view, kRegionCount, "summary while release is in flight");
    ExpectEq(view.hotWordMask_, 0, "claimed Regions must disappear from hot summary");

    manager.CompleteScavengingLocked(shard, 0, 64, false);
    ExpectHotWordSummaryMatches(view, kRegionCount, "summary after release rollback");
    ExpectEq(view.hotWordMask_, 0x1, "failed release restores hot summary");

    manager.MarkScavengingLocked(shard, 0, 64);
    manager.CompleteScavengingLocked(shard, 0, 64, true);
    ExpectHotWordSummaryMatches(view, kRegionCount, "summary after successful release");
    ExpectEq(view.hotWordMask_, 0, "successfully released Regions remain cold");

    FreeHeap(heap, kHeapBytes);
}

void TestScavengeSkipsColdViewsAndUsesSavedCursor()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 16ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kRegionCount = kRegionsPerSegment * 3;
    constexpr size_t kHeapBytes = static_cast<size_t>(kRegionCount) * kRegionSize;

    void* heap = AllocateHeap(kHeapBytes);
    FreeManager manager;
    manager.Initialize(
        kPageSize,
        kUnitsPerRegion,
        kRegionCount,
        kRegionCount,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kSegmentSize,
        1);

    FreeManager::FreeManagerShard& shard = manager.GetShard(0);
    {
        std::lock_guard<std::mutex> lock(shard.shardMutex_);
        manager.MarkScavengingLocked(shard, kRegionsPerSegment, kRegionsPerSegment * 2);
        manager.CompleteScavengingLocked(shard, kRegionsPerSegment, kRegionsPerSegment * 2, true);
    }

    ExpectEq(manager.ScavengeFreeRegionsUpTo(kRegionSize), kRegionSize,
             "sparse slice should release one Region from the only hot view");
    if (shard.scavengeViewCursor_ >= shard.ownedViews_.size()) {
        std::fprintf(stderr, "saved Scavenge view cursor escaped owned view bounds\n");
        std::abort();
    }
    if (shard.scavengeWordCursor_ >= shard.ownedViews_[shard.scavengeViewCursor_].bitmapWordCount_) {
        std::fprintf(stderr, "saved Scavenge word cursor escaped bitmap word bounds\n");
        std::abort();
    }

    FreeHeap(heap, kHeapBytes);
}

void TestScavengeCoalescesOnlyAdjacentRegions()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr uint32_t kRegionCount = 130;
    constexpr size_t kHeapBytes = static_cast<size_t>(kRegionCount) * kRegionSize;

    void* heap = AllocateHeap(kHeapBytes);
    FreeManager manager;
    manager.Initialize(
        kPageSize,
        kUnitsPerRegion,
        kRegionCount,
        kRegionCount,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kHeapBytes,
        1);

    ExpectTake(manager, kRegionCount, 0, 0, "test setup should allocate the complete view");
    manager.ReclaimRegionsReleased(63, 2);
    ExpectEq(manager.ScavengeFreeRegionsUpTo(2 * kRegionSize), 2 * kRegionSize,
             "adjacent cross-word Regions should be released");

    manager.ReclaimRegionsReleased(62, 1);
    manager.ReclaimRegionsReleased(64, 1);
    ExpectEq(manager.ScavengeFreeRegionsUpTo(2 * kRegionSize), 2 * kRegionSize,
             "fragmented hot Regions should both be released");

    FreeHeap(heap, kHeapBytes);
}

void TestHotWordSummaryFixedSeedStateMachine()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr uint32_t kRegionCount = 130;
    constexpr size_t kHeapBytes = static_cast<size_t>(kRegionCount) * kRegionSize;

    void* heap = AllocateHeap(kHeapBytes);
    FreeManager manager;
    manager.Initialize(
        kPageSize,
        kUnitsPerRegion,
        kRegionCount,
        kRegionCount,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kHeapBytes,
        1);

    uint64_t seed = 0x9e3779b97f4a7c15ULL;
    auto next = [&seed]() {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };

    FreeManager::FreeManagerShard& shard = manager.GetShard(0);
    {
        std::lock_guard<std::mutex> lock(shard.shardMutex_);
        FreeManager::OwnedSegmentView& view = shard.ownedViews_.front();
        for (size_t step = 0; step < 500; ++step) {
            size_t localIdx = static_cast<size_t>(next() % kRegionCount);
            FreeManager::RegionIndex regionIdx = static_cast<FreeManager::RegionIndex>(localIdx);
            bool allocated = FreeManager::TestBit(view.allocBits_.data(), localIdx);
            bool scavenged = FreeManager::TestBit(view.scavengedBits_.data(), localIdx);
            bool scavenging = FreeManager::TestBit(view.scavengingBits_.data(), localIdx);
            if (scavenging) {
                std::fprintf(stderr, "state-machine step began with an in-flight Region\n");
                std::abort();
            }

            if (!allocated && !scavenged) {
                if ((next() & 1ULL) == 0) {
                    bool reusedScavenged = false;
                    manager.MarkAllocatedLocked(shard, regionIdx, 1, reusedScavenged);
                } else {
                    manager.MarkScavengingLocked(shard, regionIdx, 1);
                    manager.CompleteScavengingLocked(shard, regionIdx, 1, (next() & 1ULL) != 0);
                }
            } else if (!allocated) {
                bool reusedScavenged = false;
                manager.MarkAllocatedLocked(shard, regionIdx, 1, reusedScavenged);
            } else {
                manager.MarkFreeLocked(shard, regionIdx, 1);
            }

            size_t expectedFree = 0;
            size_t expectedScavenged = 0;
            size_t expectedScavenging = 0;
            for (size_t probe = 0; probe < kRegionCount; ++probe) {
                expectedFree += FreeManager::TestBit(view.allocBits_.data(), probe) ? 0 : 1;
                expectedScavenged += FreeManager::TestBit(view.scavengedBits_.data(), probe) ? 1 : 0;
                expectedScavenging += FreeManager::TestBit(view.scavengingBits_.data(), probe) ? 1 : 0;
            }
            ExpectEq(shard.freeRegionCount_, expectedFree, "fixed-seed free count");
            ExpectEq(shard.scavengedRegionCount_, expectedScavenged, "fixed-seed scavenged count");
            ExpectEq(shard.scavengingRegionCount_, expectedScavenging, "fixed-seed scavenging count");
            ExpectHotWordSummaryMatches(view, kRegionCount, "fixed-seed hot-word summary");
        }
    }

    if (shard.scavengeViewCursor_ >= shard.ownedViews_.size() ||
        shard.scavengeWordCursor_ >= shard.ownedViews_[shard.scavengeViewCursor_].bitmapWordCount_) {
        std::fprintf(stderr, "fixed-seed state machine left an invalid Scavenge cursor\n");
        std::abort();
    }
    FreeHeap(heap, kHeapBytes);
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

void TestColdReuseCounterTracksClearedScavengedBitsOnce()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr uint32_t kRegionCount = 3;
    constexpr size_t kHeapBytes = static_cast<size_t>(kRegionCount) * kRegionSize;

    void* heap = AllocateHeap(kHeapBytes);
    FreeManager manager;
    manager.Initialize(
        kPageSize,
        kUnitsPerRegion,
        kRegionCount,
        kRegionCount,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kHeapBytes,
        1);

    ExpectEq(manager.GetReusedScavengedRegionCount(), 0,
             "fresh FreeManager must have no cold Region reuse");
    ExpectEq(manager.ScavengeFreeRegionsUpTo(kHeapBytes), kHeapBytes,
             "all test Regions should become scavenged");
    ExpectEq(manager.GetScavengedRegionCount(0), kRegionCount,
             "all test Regions should carry scavenged bits");

    ExpectTake(manager, 2, 0, 0,
               "contiguous allocation should reuse the first two scavenged Regions");
    ExpectEq(manager.GetReusedScavengedRegionCount(), 2,
             "contiguous cold reuse must add its exact scavenged count");

    ExpectTake(manager, 1, 0, 2,
               "single allocation should reuse the remaining scavenged Region");
    ExpectEq(manager.GetReusedScavengedRegionCount(), 3,
             "single cold reuse must add one after clearing its scavenged bit");

    manager.ReclaimRegionsReleased(0, kRegionCount);
    ExpectTake(manager, 1, 0, 0,
               "hot Region reuse should remain allocatable after reclaim");
    ExpectEq(manager.GetReusedScavengedRegionCount(), 3,
             "hot Region reuse must not increment the cold reuse counter again");

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

void TestAppleReleaseRemapsZeroPagesWithoutPreZero()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr uint32_t kRegionCount = 1;

    void* heap = AllocateHeap(kRegionSize);
    std::memset(heap, 0xa5, kRegionSize);
    FreeManager manager;
    manager.Initialize(kPageSize, kUnitsPerRegion, kRegionCount, kRegionCount,
                       reinterpret_cast<uintptr_t>(heap),
                       reinterpret_cast<uintptr_t>(heap) + kRegionSize,
                       reinterpret_cast<uintptr_t>(heap) + kRegionSize,
                       kRegionSize, 1);

    gMemorySetCalls.store(0, std::memory_order_relaxed);
    ExpectEq(manager.ScavengeFreeRegionsUpTo(kRegionSize), kRegionSize,
             "fresh anonymous mapping should release one Region");
    ExpectEq(gMemorySetCalls.load(std::memory_order_relaxed), 0,
             "MAP_FIXED anonymous remap must not pre-zero pages in user space");

    const auto* bytes = static_cast<const unsigned char*>(heap);
    for (size_t i = 0; i < kRegionSize; ++i) {
        if (bytes[i] != 0) {
            std::fprintf(stderr, "released anonymous page must read back as zero: offset=%zu\n", i);
            std::abort();
        }
    }
    FreeHeap(heap, kRegionSize);
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

void TestBackgroundScavengeHasNoEpochReleaseCap()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 16ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr size_t kHeapBytes = kSegmentSize * 4;

    void* heap = AllocateHeap(kHeapBytes);
    FreeManager manager;
    manager.Initialize(kPageSize, kUnitsPerRegion, kRegionsPerSegment * 4,
                       kRegionsPerSegment * 4,
                       reinterpret_cast<uintptr_t>(heap),
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
                       kSegmentSize, 1);

    ExpectTake(manager, kRegionsPerSegment * 4, 0, 0,
               "test setup should allocate all Segments");
    for (uint32_t idx = 0; idx < kRegionsPerSegment * 4; idx += 2) {
        manager.ReclaimRegionsReleased(idx, 1);
    }

    ExpectEq(manager.ScavengeAgedRegionsUpTo(kSegmentSize), 0,
             "newly freed Regions must age through one GC epoch before release");
    manager.AdvanceScavengeEpoch(2);
    ExpectEq(manager.ScavengeAgedRegionsUpTo(kSegmentSize - kRegionSize),
             kSegmentSize - kRegionSize,
             "a sub-Segment budget must release available aged Regions");
    ExpectEq(manager.ScavengeAgedRegionsUpTo(kSegmentSize), kSegmentSize,
             "the next slice must release more aged Regions in the same epoch");
    ExpectEq(manager.ScavengeAgedRegionsUpTo(kRegionSize), kRegionSize,
             "the final aged Region must remain releasable in the same epoch");

    FreeHeap(heap, kHeapBytes);
}

void TestBackgroundScavengeReleasesSubSegmentAgedSupply()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 16ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;

    void* heap = AllocateHeap(kSegmentSize);
    FreeManager manager;
    manager.Initialize(kPageSize, kUnitsPerRegion, kRegionsPerSegment,
                       kRegionsPerSegment,
                       reinterpret_cast<uintptr_t>(heap),
                       reinterpret_cast<uintptr_t>(heap) + kSegmentSize,
                       reinterpret_cast<uintptr_t>(heap) + kSegmentSize,
                       kSegmentSize, 1);

    ExpectTake(manager, kRegionsPerSegment, 0, 0,
               "test setup should allocate the whole Segment");
    manager.ReclaimRegionsReleased(0, 1);
    ExpectEq(manager.ScavengeAgedRegionsUpTo(kRegionSize), 0,
             "current-epoch hot-free Region must not be released");

    manager.AdvanceScavengeEpoch(2);
    ExpectEq(manager.ScavengeAgedRegionsUpTo(kRegionSize), kRegionSize,
             "one aged Region must be releasable below the Segment-sized floor");

    FreeHeap(heap, kSegmentSize);
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

    manager.shards_[0].scavengeViewCursor_ = 1;
    manager.shards_[0].scavengeWordCursor_ =
        manager.shards_[0].ownedViews_[1].bitmapWordCount_ - 1;
    gForceReleaseFailure = true;
    manager.ShrinkHeap();
    gForceReleaseFailure = false;
    ExpectEq(manager.GetCommittedRegionCount(), kInitialRegions,
             "failed tail release must not shrink committed topology");
    ExpectEq(manager.GetOwnedSegmentCount(0), 2,
             "failed tail release must not detach segment metadata");
    ExpectEq(manager.shards_[0].scavengeViewCursor_, 1,
             "failed shrink must preserve the saved Scavenge view cursor");

    manager.ShrinkHeap();
    ExpectEq(manager.GetCommittedRegionCount(), kRegionsPerSegment,
             "successful tail release should shrink one segment");
    ExpectEq(manager.GetOwnedSegmentCount(0), 1,
             "successful tail release should detach one segment view");
    ExpectEq(manager.shards_[0].scavengeViewCursor_, 0,
             "successful shrink must clamp the saved Scavenge view cursor");
    if (manager.shards_[0].scavengeWordCursor_ >=
        manager.shards_[0].ownedViews_[0].bitmapWordCount_) {
        std::fprintf(stderr, "successful shrink left Scavenge word cursor out of bounds\n");
        std::abort();
    }

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
    TestHotWordSummaryInitializationAndTransitions();
    TestScavengeSkipsColdViewsAndUsesSavedCursor();
    TestScavengeCoalescesOnlyAdjacentRegions();
    TestHotWordSummaryFixedSeedStateMachine();
    TestPerShardCountsTrackOwnerAndBorrowedBatchConsumption();
    TestScavengeRotatesAcrossOwnerShards();
    TestColdReuseCounterTracksClearedScavengedBitsOnce();
    TestScavengeSkipsBusyShardWithTryLock();
    TestScavengeReleasesOutsideShardLock();
    TestAppleReleaseRemapsZeroPagesWithoutPreZero();
    TestScavengeFailureRollsBackToHotFree();
    TestWholeSegmentReleaseUsesTwoPhasePublication();
    TestBackgroundScavengeHasNoEpochReleaseCap();
    TestBackgroundScavengeReleasesSubSegmentAgedSupply();
    TestShrinkKeepsTopologyWhenReleaseFails();
    TestClaimedRegionCannotBeAllocatedDuringRelease();
    return 0;
}
