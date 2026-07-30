// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_BARRIER_MARK_BUFFER_H
#define MRT_MS_BARRIER_MARK_BUFFER_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace MapleRuntime {

constexpr uint32_t kMSBarrierMarkBufferCapacity = 32;
constexpr uint32_t kMSBarrierMarkBufferSetCount = 8;
constexpr uint32_t kMSBarrierMarkBufferWays = 4;
constexpr uint32_t kMSBarrierMarkBufferHashMultiplier = 5;
constexpr uint32_t kMSBarrierMarkBufferPendingBitFlushThreshold = 128;
constexpr uint8_t kMSBarrierMarkBufferInvalidEntry = UINT8_C(0xff);

struct alignas(8) MSBarrierMarkEntry {
    uint64_t wordKey;
    uint64_t mask;
};

// Compiler-visible POD ABI. Keep entries first: AllocBufferMS is 64-byte
// aligned and embeds this object at offset 576, so every four-way set occupies
// one complete cache line.
struct alignas(8) MSBarrierMarkBuffer {
    MSBarrierMarkEntry entries[kMSBarrierMarkBufferCapacity];
    uint32_t occupancy;
    uint32_t pendingBitCount;
    uint8_t lastEntry;
    uint8_t reserved[3];
};

enum class MSBarrierMarkAddResult : uint8_t {
    ALREADY_PENDING,
    ADDED,
    NEEDS_FLUSH
};

inline uint64_t MakeMSBarrierMarkWordKey(uint32_t regionIdx, uint32_t wordIdx)
{
    return (static_cast<uint64_t>(regionIdx) << 6) |
        static_cast<uint64_t>(wordIdx);
}

inline uint32_t GetMSBarrierMarkRegionIndex(uint64_t wordKey)
{
    return static_cast<uint32_t>(wordKey >> 6);
}

inline uint32_t GetMSBarrierMarkWordIndex(uint64_t wordKey)
{
    return static_cast<uint32_t>(wordKey & UINT64_C(63));
}

inline uint32_t GetMSBarrierMarkBufferSet(uint64_t wordKey)
{
    uint32_t regionIdx = GetMSBarrierMarkRegionIndex(wordKey);
    uint32_t wordIdx = GetMSBarrierMarkWordIndex(wordKey);
    return (regionIdx ^ (wordIdx * kMSBarrierMarkBufferHashMultiplier)) &
        (kMSBarrierMarkBufferSetCount - 1);
}

inline bool IsMSBarrierMarkBufferEmpty(const MSBarrierMarkBuffer& buffer)
{
    return buffer.occupancy == 0;
}

inline void ClearMSBarrierMarkPendingBits(MSBarrierMarkBuffer& buffer)
{
    buffer.occupancy = 0;
    buffer.pendingBitCount = 0;
    buffer.lastEntry = kMSBarrierMarkBufferInvalidEntry;
}

inline void ResetMSBarrierMarkBuffer(MSBarrierMarkBuffer& buffer)
{
    ClearMSBarrierMarkPendingBits(buffer);
    buffer.reserved[0] = 0;
    buffer.reserved[1] = 0;
    buffer.reserved[2] = 0;
}

inline uint8_t FindMSBarrierMarkEntry(
    const MSBarrierMarkBuffer& buffer, uint64_t wordKey)
{
    uint8_t last = buffer.lastEntry;
    if (last < kMSBarrierMarkBufferCapacity &&
        (buffer.occupancy & (UINT32_C(1) << last)) != 0 &&
        buffer.entries[last].wordKey == wordKey) {
        return last;
    }

    uint32_t first =
        GetMSBarrierMarkBufferSet(wordKey) * kMSBarrierMarkBufferWays;
    uint32_t occupiedWays =
        (buffer.occupancy >> first) &
        ((UINT32_C(1) << kMSBarrierMarkBufferWays) - 1);
    while (occupiedWays != 0) {
        uint32_t way =
            static_cast<uint32_t>(__builtin_ctz(occupiedWays));
        uint32_t entryIdx = first + way;
        if (buffer.entries[entryIdx].wordKey == wordKey) {
            return static_cast<uint8_t>(entryIdx);
        }
        occupiedWays &= occupiedWays - 1;
    }
    return kMSBarrierMarkBufferInvalidEntry;
}

inline MSBarrierMarkAddResult TryAddMSBarrierMarkPendingBits(
    MSBarrierMarkBuffer& buffer, uint64_t wordKey, uint64_t bitMask)
{
    uint8_t existing = FindMSBarrierMarkEntry(buffer, wordKey);
    if (existing != kMSBarrierMarkBufferInvalidEntry) {
        MSBarrierMarkEntry& entry = buffer.entries[existing];
        uint64_t newBits = bitMask & ~entry.mask;
        if (newBits == 0) {
            buffer.lastEntry = existing;
            return MSBarrierMarkAddResult::ALREADY_PENDING;
        }
        uint32_t addedBits =
            static_cast<uint32_t>(__builtin_popcountll(newBits));
        if (buffer.pendingBitCount + addedBits >
            kMSBarrierMarkBufferPendingBitFlushThreshold) {
            return MSBarrierMarkAddResult::NEEDS_FLUSH;
        }
        entry.mask |= newBits;
        buffer.pendingBitCount += addedBits;
        buffer.lastEntry = existing;
        return MSBarrierMarkAddResult::ADDED;
    }

    uint32_t addedBits =
        static_cast<uint32_t>(__builtin_popcountll(bitMask));
    if (addedBits == 0) {
        return MSBarrierMarkAddResult::ALREADY_PENDING;
    }
    if (buffer.pendingBitCount + addedBits >
        kMSBarrierMarkBufferPendingBitFlushThreshold) {
        return MSBarrierMarkAddResult::NEEDS_FLUSH;
    }

    uint32_t first =
        GetMSBarrierMarkBufferSet(wordKey) * kMSBarrierMarkBufferWays;
    uint32_t occupiedWays =
        (buffer.occupancy >> first) &
        ((UINT32_C(1) << kMSBarrierMarkBufferWays) - 1);
    uint32_t allWays = (UINT32_C(1) << kMSBarrierMarkBufferWays) - 1;
    if (occupiedWays == allWays) {
        return MSBarrierMarkAddResult::NEEDS_FLUSH;
    }

    uint32_t emptyWay =
        static_cast<uint32_t>(__builtin_ctz((~occupiedWays) & allWays));
    uint32_t entryIdx = first + emptyWay;
    MSBarrierMarkEntry& entry = buffer.entries[entryIdx];
    entry.wordKey = wordKey;
    entry.mask = bitMask;
    buffer.occupancy |= UINT32_C(1) << entryIdx;
    buffer.pendingBitCount += addedBits;
    buffer.lastEntry = static_cast<uint8_t>(entryIdx);
    return MSBarrierMarkAddResult::ADDED;
}

static_assert(std::is_standard_layout<MSBarrierMarkEntry>::value,
              "MSBarrierMarkEntry must remain standard-layout");
static_assert(std::is_trivially_copyable<MSBarrierMarkEntry>::value,
              "MSBarrierMarkEntry must remain trivially copyable");
static_assert(sizeof(MSBarrierMarkEntry) == 16,
              "compiler ABI: MSBarrierMarkEntry size");
static_assert(offsetof(MSBarrierMarkEntry, wordKey) == 0,
              "compiler ABI: MSBarrierMarkEntry::wordKey");
static_assert(offsetof(MSBarrierMarkEntry, mask) == 8,
              "compiler ABI: MSBarrierMarkEntry::mask");
static_assert(std::is_standard_layout<MSBarrierMarkBuffer>::value,
              "MSBarrierMarkBuffer must remain standard-layout");
static_assert(std::is_trivially_copyable<MSBarrierMarkBuffer>::value,
              "MSBarrierMarkBuffer must remain trivially copyable");
static_assert(alignof(MSBarrierMarkBuffer) == 8,
              "compiler ABI: MSBarrierMarkBuffer alignment");
static_assert(sizeof(MSBarrierMarkBuffer) == 528,
              "compiler ABI: MSBarrierMarkBuffer size");
static_assert(offsetof(MSBarrierMarkBuffer, entries) == 0,
              "compiler ABI: MSBarrierMarkBuffer::entries");
static_assert(offsetof(MSBarrierMarkBuffer, occupancy) == 512,
              "compiler ABI: MSBarrierMarkBuffer::occupancy");
static_assert(offsetof(MSBarrierMarkBuffer, pendingBitCount) == 516,
              "compiler ABI: MSBarrierMarkBuffer::pendingBitCount");
static_assert(offsetof(MSBarrierMarkBuffer, lastEntry) == 520,
              "compiler ABI: MSBarrierMarkBuffer::lastEntry");
static_assert(
    (kMSBarrierMarkBufferSetCount &
     (kMSBarrierMarkBufferSetCount - 1)) == 0,
    "MSBarrierMarkBuffer set count must be a power of two");
static_assert(
    kMSBarrierMarkBufferSetCount * kMSBarrierMarkBufferWays ==
        kMSBarrierMarkBufferCapacity,
    "MSBarrierMarkBuffer geometry mismatch");

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_BARRIER_MARK_BUFFER_H
