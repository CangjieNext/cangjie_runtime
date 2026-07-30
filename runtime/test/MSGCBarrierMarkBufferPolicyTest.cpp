#include <cassert>
#include <cstdint>

#define MRT_USE_MARK_SWEEP 1

#include "src/Heap/MBarrier/MSBarrierMarkBuffer.h"

int main()
{
    using namespace MapleRuntime;

    MSBarrierMarkBuffer buffer {};
    ResetMSBarrierMarkBuffer(buffer);
    assert(IsMSBarrierMarkBufferEmpty(buffer));
    assert(buffer.lastEntry == kMSBarrierMarkBufferInvalidEntry);

    const uint64_t firstKey = MakeMSBarrierMarkWordKey(3, 2);
    assert(GetMSBarrierMarkRegionIndex(firstKey) == 3);
    assert(GetMSBarrierMarkWordIndex(firstKey) == 2);
    assert(GetMSBarrierMarkBufferSet(firstKey) ==
           ((3U ^ (2U * kMSBarrierMarkBufferHashMultiplier)) & 7U));

    assert(TryAddMSBarrierMarkPendingBits(buffer, firstKey, UINT64_C(1)) ==
           MSBarrierMarkAddResult::ADDED);
    assert(buffer.pendingBitCount == 1);
    assert(__builtin_popcount(buffer.occupancy) == 1);
    assert(TryAddMSBarrierMarkPendingBits(buffer, firstKey, UINT64_C(1)) ==
           MSBarrierMarkAddResult::ALREADY_PENDING);
    assert(buffer.pendingBitCount == 1);
    assert(TryAddMSBarrierMarkPendingBits(buffer, firstKey, UINT64_C(2)) ==
           MSBarrierMarkAddResult::ADDED);
    assert(buffer.pendingBitCount == 2);
    assert(__builtin_popcount(buffer.occupancy) == 1);

    ResetMSBarrierMarkBuffer(buffer);
    for (uint32_t way = 0; way < 4; ++way) {
        const uint64_t key = MakeMSBarrierMarkWordKey(1 + 8 * way, 0);
        assert(GetMSBarrierMarkBufferSet(key) == 1);
        assert(TryAddMSBarrierMarkPendingBits(
                   buffer, key, UINT64_C(1)) ==
               MSBarrierMarkAddResult::ADDED);
    }
    const uint64_t conflictKey = MakeMSBarrierMarkWordKey(33, 0);
    assert(GetMSBarrierMarkBufferSet(conflictKey) == 1);
    assert(TryAddMSBarrierMarkPendingBits(
               buffer, conflictKey, UINT64_C(1)) ==
           MSBarrierMarkAddResult::NEEDS_FLUSH);
    assert(buffer.pendingBitCount == 4);
    assert(__builtin_popcount(buffer.occupancy) == 4);

    ResetMSBarrierMarkBuffer(buffer);
    for (uint32_t set = 0; set < kMSBarrierMarkBufferSetCount; ++set) {
        for (uint32_t way = 0; way < kMSBarrierMarkBufferWays; ++way) {
            const uint64_t key =
                MakeMSBarrierMarkWordKey(set + 8 * way, 0);
            assert(GetMSBarrierMarkBufferSet(key) == set);
            assert(TryAddMSBarrierMarkPendingBits(
                       buffer, key, UINT64_C(0xf)) ==
                   MSBarrierMarkAddResult::ADDED);
        }
    }
    assert(buffer.pendingBitCount ==
           kMSBarrierMarkBufferPendingBitFlushThreshold);
    assert(__builtin_popcount(buffer.occupancy) ==
           kMSBarrierMarkBufferCapacity);

    const uint64_t existingKey = MakeMSBarrierMarkWordKey(0, 0);
    assert(TryAddMSBarrierMarkPendingBits(
               buffer, existingKey, UINT64_C(1)) ==
           MSBarrierMarkAddResult::ALREADY_PENDING);
    assert(TryAddMSBarrierMarkPendingBits(
               buffer, existingKey, UINT64_C(0x10)) ==
           MSBarrierMarkAddResult::NEEDS_FLUSH);
    assert(buffer.pendingBitCount ==
           kMSBarrierMarkBufferPendingBitFlushThreshold);

    return 0;
}
