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

void ExpectEq(size_t actual, size_t expected, const char* message)
{
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected=%zu actual=%zu\n", message, expected, actual);
        std::abort();
    }
}

void ExpectTrue(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

void ExpectNePtr(const void* lhs, const void* rhs, const char* message)
{
    if (lhs == rhs) {
        std::fprintf(stderr, "%s: both=%p\n", message, lhs);
        std::abort();
    }
}

void TestOwnedSegmentsCarryIndependentMetadataBlocks()
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
        kPageSize,
        kUnitsPerRegion,
        kInitialRegions,
        kInitialRegions,
        reinterpret_cast<uintptr_t>(heap),
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        reinterpret_cast<uintptr_t>(heap) + kHeapBytes,
        kSegmentSize,
        2);

    const auto& shard0Seg = manager.shards_[0].ownedViews_.front();
    const auto& shard1Seg = manager.shards_[1].ownedViews_.front();
    ExpectEq(shard0Seg.allocBits_.size(), kMetadataWordsPerSegment, "shard0 segment alloc bitmap words");
    ExpectEq(shard0Seg.scavengedBits_.size(), kMetadataWordsPerSegment, "shard0 segment scavenged bitmap words");
    ExpectEq(shard0Seg.scavengingBits_.size(), kMetadataWordsPerSegment, "shard0 segment scavenging bitmap words");
    ExpectEq(shard0Seg.chunkSummaries_.size(), kMetadataWordsPerSegment, "shard0 segment summary chunks");
    ExpectEq(shard1Seg.allocBits_.size(), kMetadataWordsPerSegment, "shard1 segment alloc bitmap words");
    ExpectEq(shard1Seg.scavengedBits_.size(), kMetadataWordsPerSegment, "shard1 segment scavenged bitmap words");
    ExpectEq(shard1Seg.scavengingBits_.size(), kMetadataWordsPerSegment, "shard1 segment scavenging bitmap words");
    ExpectEq(shard1Seg.chunkSummaries_.size(), kMetadataWordsPerSegment, "shard1 segment summary chunks");
    ExpectNePtr(shard0Seg.allocBits_.data(), shard1Seg.allocBits_.data(), "per-shard alloc bitmaps must not alias");
    ExpectNePtr(shard0Seg.scavengedBits_.data(), shard1Seg.scavengedBits_.data(), "per-shard scavenged bitmaps must not alias");
    ExpectNePtr(shard0Seg.scavengingBits_.data(), shard1Seg.scavengingBits_.data(), "per-shard scavenging bitmaps must not alias");
    ExpectNePtr(shard0Seg.chunkSummaries_.data(), shard1Seg.chunkSummaries_.data(), "per-shard summaries must not alias");
    ExpectTrue(shard0Seg.allocBits_[0] == 0, "fresh segment alloc bitmap should start free");
    ExpectTrue(shard0Seg.scavengedBits_[0] == 0, "fresh segment scavenged bitmap should start clear");
    ExpectTrue(shard0Seg.scavengingBits_[0] == 0, "fresh segment scavenging bitmap should start clear");
    ExpectTrue(shard0Seg.chunkSummaries_[0].max != 0, "fresh segment summary should record free run");
    ExpectEq(shard0Seg.freeEpochs_.size(), kRegionsPerSegment,
             "each Region must carry its own Scavenge free epoch");
    ExpectEq(shard0Seg.freeEpochs_[0], 1,
             "fresh hot-free Regions must start aging in the initial Scavenge epoch");

    std::free(heap);
}

} // namespace

int main()
{
    TestOwnedSegmentsCarryIndependentMetadataBlocks();
    return 0;
}
