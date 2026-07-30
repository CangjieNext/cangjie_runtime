#include <cstddef>
#include <cstdint>
#include <type_traits>

#define MRT_USE_MARK_SWEEP 1

#include "src/Heap/MBarrier/MSBarrierMarkBuffer.h"

int main()
{
    using namespace MapleRuntime;

    static_assert(std::is_standard_layout<MSBarrierMarkEntry>::value,
                  "MSBarrierMarkEntry must be standard-layout");
    static_assert(std::is_trivially_copyable<MSBarrierMarkEntry>::value,
                  "MSBarrierMarkEntry must be trivially copyable");
    static_assert(alignof(MSBarrierMarkEntry) == 8,
                  "MSBarrierMarkEntry alignment");
    static_assert(sizeof(MSBarrierMarkEntry) == 16,
                  "MSBarrierMarkEntry size");
    static_assert(offsetof(MSBarrierMarkEntry, wordKey) == 0,
                  "MSBarrierMarkEntry::wordKey offset");
    static_assert(offsetof(MSBarrierMarkEntry, mask) == 8,
                  "MSBarrierMarkEntry::mask offset");

    static_assert(std::is_standard_layout<MSBarrierMarkBuffer>::value,
                  "MSBarrierMarkBuffer must be standard-layout");
    static_assert(std::is_trivially_copyable<MSBarrierMarkBuffer>::value,
                  "MSBarrierMarkBuffer must be trivially copyable");
    static_assert(alignof(MSBarrierMarkBuffer) == 8,
                  "MSBarrierMarkBuffer alignment");
    static_assert(sizeof(MSBarrierMarkBuffer) == 528,
                  "MSBarrierMarkBuffer size");
    static_assert(offsetof(MSBarrierMarkBuffer, entries) == 0,
                  "MSBarrierMarkBuffer::entries offset");
    static_assert(offsetof(MSBarrierMarkBuffer, occupancy) == 512,
                  "MSBarrierMarkBuffer::occupancy offset");
    static_assert(offsetof(MSBarrierMarkBuffer, pendingBitCount) == 516,
                  "MSBarrierMarkBuffer::pendingBitCount offset");
    static_assert(offsetof(MSBarrierMarkBuffer, lastEntry) == 520,
                  "MSBarrierMarkBuffer::lastEntry offset");
    static_assert(kMSBarrierMarkBufferCapacity == 32,
                  "MSBarrierMarkBuffer capacity");
    static_assert(kMSBarrierMarkBufferSetCount == 8,
                  "MSBarrierMarkBuffer set count");
    static_assert(kMSBarrierMarkBufferWays == 4,
                  "MSBarrierMarkBuffer ways");
    static_assert(kMSBarrierMarkBufferHashMultiplier == 5,
                  "MSBarrierMarkBuffer hash multiplier");
    static_assert(kMSBarrierMarkBufferPendingBitFlushThreshold == 128,
                  "MSBarrierMarkBuffer pending-bit threshold");
    static_assert(kMSBarrierMarkBufferInvalidEntry == 0xff,
                  "MSBarrierMarkBuffer invalid entry sentinel");

    return 0;
}
