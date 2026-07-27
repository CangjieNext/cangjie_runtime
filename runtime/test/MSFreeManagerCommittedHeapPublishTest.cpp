#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

void ExpectTrue(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
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

class RecordingHeapRangeHandler final : public FreeManager::HeapRangeHandler {
public:
    void InitializeFreshRegions(FreeManager::RegionIndex, FreeManager::RegionCount) override {}

    void OnCommittedHeapExtended(uintptr_t newCommittedEnd) override
    {
        observedEnds_.push_back(newCommittedEnd);
    }

    const std::vector<uintptr_t>& ObservedEnds() const
    {
        return observedEnds_;
    }

private:
    std::vector<uintptr_t> observedEnds_;
};

void TestGrowPublishesCommittedHeapEnd()
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
        1);

    RecordingHeapRangeHandler handler;
    bool grew = manager.GrowOneSegment(0, handler);
    ExpectTrue(grew, "GrowOneSegment should succeed");
    manager.ShrinkHeap();

    const std::vector<uintptr_t>& observedEnds = handler.ObservedEnds();
    ExpectEqSize(observedEnds.size(), 1, "grow should publish one committed heap end");

    uintptr_t heapStart = reinterpret_cast<uintptr_t>(heap);
    uintptr_t grownEnd = heapStart + kHeapBytes;
    uintptr_t shrunkEnd = heapStart + static_cast<size_t>(kInitialRegions) * kRegionSize;

    ExpectEqSize(observedEnds[0], grownEnd, "grow should publish enlarged committed end");
    ExpectEqSize(manager.GetCommittedEnd(), shrunkEnd, "manager committed end should match shrink end");

    std::free(heap);
}

} // namespace

int main()
{
    TestGrowPublishesCommittedHeapEnd();
    return 0;
}
