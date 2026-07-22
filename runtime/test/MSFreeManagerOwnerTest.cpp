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

void ExpectEq(uint16_t actual, uint16_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%u actual=%u\n", message, expected, actual);
        std::abort();
    }
}

void ExpectEqSize(size_t actual, size_t expected, const char* message)
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

void TestInitialSegmentsDefaultToOwnerZero()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr size_t kMetadataWordsPerSegment =
        (kRegionsPerSegment + FreeManager::kChunkBits - 1) / FreeManager::kChunkBits;
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
        4);

    ExpectEq(manager.ResolveOwnerShardId(0), 0, "first segment owner");
    ExpectEq(manager.ResolveOwnerShardId(kRegionsPerSegment), 1, "second segment owner");
    ExpectEqSize(manager.GetOwnedSegmentCount(0), 1, "shard0 initial owned segment count");
    ExpectEqSize(manager.GetOwnedSegmentCount(1), 1, "shard1 initial owned segment count");
    ExpectEqSize(manager.GetOwnedSegmentCount(2), 0, "shard2 initial owned segment count");
    ExpectEqSize(manager.shards_[0].ownedViews_.size(), 1, "shard0 initial owned view count");
    ExpectEqSize(manager.shards_[1].ownedViews_.size(), 1, "shard1 initial owned view count");
    ExpectEqSize(manager.shards_[0].ownedViews_.front().allocBits_.size(),
                 kMetadataWordsPerSegment,
                 "shard0 initial bitmap word count");
    ExpectEqSize(manager.shards_[1].ownedViews_.front().allocBits_.size(),
                 kMetadataWordsPerSegment,
                 "shard1 initial bitmap word count");
    ExpectEqSize(manager.shards_[0].ownedViews_.front().chunkSummaries_.size(),
                 kMetadataWordsPerSegment,
                 "shard0 initial summary chunk count");
    ExpectEqSize(manager.shards_[1].ownedViews_.front().chunkSummaries_.size(),
                 kMetadataWordsPerSegment,
                 "shard1 initial summary chunk count");
    ExpectEqSize(manager.shards_[0].takeCursor_, 0, "shard0 initial take cursor");
    ExpectEqSize(manager.shards_[1].takeCursor_, kRegionsPerSegment, "shard1 initial take cursor");
    ExpectEqSize(manager.shards_[0].reclaimCursor_, kInitialRegions, "shard0 initial reclaim cursor");
    ExpectEqSize(manager.shards_[1].reclaimCursor_, kInitialRegions, "shard1 initial reclaim cursor");

    ExpectTake(manager, 0, 0, "shard0 first allocation");
    ExpectEqSize(manager.shards_[0].takeCursor_, 1, "shard0 take cursor should advance independently");
    ExpectEqSize(manager.shards_[1].takeCursor_, kRegionsPerSegment, "shard1 take cursor should stay untouched");

    ExpectTake(manager, 1, kRegionsPerSegment, "shard1 first allocation");
    ExpectEqSize(manager.shards_[0].takeCursor_, 1, "shard0 take cursor should remain independent after shard1 alloc");
    ExpectEqSize(manager.shards_[1].takeCursor_, kRegionsPerSegment + 1, "shard1 take cursor should advance independently");

    manager.ReclaimRegionsReleased(0, 1);
    ExpectEqSize(manager.shards_[0].reclaimCursor_, 0, "shard0 reclaim cursor should move on local reclaim");
    ExpectEqSize(manager.shards_[1].reclaimCursor_, kInitialRegions, "shard1 reclaim cursor should remain untouched");

    std::free(heap);
}

class NoopHeapRangeHandler final : public FreeManager::HeapRangeHandler {
public:
    void InitializeFreshRegions(FreeManager::RegionIndex, FreeManager::RegionCount) override {}
    void OnCommittedHeapChanged(uintptr_t) override {}
};

void TestGrowAssignsRequesterOwner()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr size_t kMetadataWordsPerSegment =
        (kRegionsPerSegment + FreeManager::kChunkBits - 1) / FreeManager::kChunkBits;
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

    NoopHeapRangeHandler handler;
    if (!manager.GrowOneSegment(3, handler)) {
        std::fprintf(stderr, "GrowOneSegment should succeed\n");
        std::abort();
    }

    ExpectEq(manager.ResolveOwnerShardId(kRegionsPerSegment), 3, "grown segment owner");
    ExpectEqSize(manager.GetOwnedSegmentCount(0), 1, "shard0 keeps original owned segment");
    ExpectEqSize(manager.GetOwnedSegmentCount(3), 1, "shard3 owns grown segment");
    ExpectEqSize(manager.shards_[0].ownedViews_.size(), 1, "shard0 keeps original owned view");
    ExpectEqSize(manager.shards_[3].ownedViews_.size(), 1, "shard3 grows one owned view");
    ExpectEqSize(manager.shards_[0].ownedViews_.front().allocBits_.size(),
                 kMetadataWordsPerSegment,
                 "shard0 bitmap word count stays per-segment");
    ExpectEqSize(manager.shards_[3].ownedViews_.front().allocBits_.size(),
                 kMetadataWordsPerSegment,
                 "shard3 bitmap word count grows with owned segment");
    ExpectEqSize(manager.shards_[0].ownedViews_.front().chunkSummaries_.size(),
                 kMetadataWordsPerSegment,
                 "shard0 summary chunk count stays per-segment");
    ExpectEqSize(manager.shards_[3].ownedViews_.front().chunkSummaries_.size(),
                 kMetadataWordsPerSegment,
                 "shard3 summary chunk count grows with owned segment");

    std::free(heap);
}

void TestShrinkKeepsOneOwnedSegmentPerShard()
{
    constexpr size_t kPageSize = 16 * 1024;
    constexpr size_t kRegionSize = 256 * 1024;
    constexpr uint32_t kUnitsPerRegion = kRegionSize / kPageSize;
    constexpr size_t kSegmentSize = 64ULL * 1024 * 1024;
    constexpr uint32_t kRegionsPerSegment = kSegmentSize / kRegionSize;
    constexpr uint32_t kInitialRegions = kRegionsPerSegment * 2;
    constexpr uint32_t kMaxRegions = kRegionsPerSegment * 4;
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
        2);

    NoopHeapRangeHandler handler;
    if (!manager.GrowOneSegment(0, handler) || !manager.GrowOneSegment(1, handler)) {
        std::fprintf(stderr, "GrowOneSegment should succeed before shrink validation\n");
        std::abort();
    }

    ExpectEqSize(manager.GetOwnedSegmentCount(0), 2, "shard0 starts shrink with two owned segments");
    ExpectEqSize(manager.GetOwnedSegmentCount(1), 2, "shard1 starts shrink with two owned segments");

    manager.ShrinkHeap(handler);

    ExpectEqSize(manager.GetCommittedRegionCount(),
                 kRegionsPerSegment * 2,
                 "shrink should keep one segment per shard committed");
    ExpectEqSize(manager.GetOwnedSegmentCount(0), 1, "shard0 should keep one owned segment after shrink");
    ExpectEqSize(manager.GetOwnedSegmentCount(1), 1, "shard1 should keep one owned segment after shrink");
    ExpectEqSize(manager.shards_[0].ownedSegments_.front(), 0, "shard0 should keep the first segment");
    ExpectEqSize(manager.shards_[1].ownedSegments_.front(), 1, "shard1 should keep the first local segment");
    ExpectEqSize(manager.shards_[0].takeCursor_, 0, "shard0 take cursor stays on remaining local segment");
    ExpectEqSize(manager.shards_[1].takeCursor_,
                 kRegionsPerSegment,
                 "shard1 take cursor stays on remaining local segment");

    std::free(heap);
}

} // namespace

int main()
{
    TestInitialSegmentsDefaultToOwnerZero();
    TestGrowAssignsRequesterOwner();
    TestShrinkKeepsOneOwnedSegmentPerShard();
    return 0;
}
