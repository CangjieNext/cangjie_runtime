// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_BATCH_H
#define MRT_MS_MARK_BATCH_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>

#include "Base/Log.h"
#include "Heap/MAllocator/MSRegionInfo.h"

namespace MapleRuntime {

struct MSMarkWordBatchEntry {
    uint64_t wordKey;
    uint64_t mask;

    uint32_t RegionIndex() const
    {
        return static_cast<uint32_t>(wordKey >> 6);
    }

    uint8_t WordIndex() const
    {
        return static_cast<uint8_t>(wordKey & UINT64_C(63));
    }
};

// Allocation-free staging for Region scheduler publication after MARK words
// have been updated. Multiple newly won bitmap words in one Region collapse
// into one pendingWordSummary RMW and one scheduler admission attempt.
// Storage is intentionally left uninitialized beyond size_: mutator slow paths
// must not clear the full scratch capacity on every add attempt.
template<size_t Capacity>
class MSPendingMarkRegionBatch {
public:
    static_assert(Capacity > 0, "pending Region batch must have storage");

    bool TryAdd(MSRegionInfo* info, size_t wordIdx)
    {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        CHECK_DETAIL(info != nullptr && wordIdx < 64,
                     "[MS-GC] invalid pending Region publication info=%p word=%zu",
                     static_cast<void*>(info), wordIdx);
#endif
        uint64_t wordMask = UINT64_C(1) << wordIdx;
        for (size_t i = 0; i < size_; ++i) {
            if (entries_[i].info == info) {
                entries_[i].wordMask |= wordMask;
                return true;
            }
        }
        if (size_ == Capacity) {
            return false;
        }
        entries_[size_].info = info;
        entries_[size_].wordMask = wordMask;
        ++size_;
        return true;
    }

    template<typename PublishRegion>
    void FlushAll(PublishRegion&& publishRegion)
    {
        for (size_t i = 0; i < size_; ++i) {
            publishRegion(entries_[i].info, entries_[i].wordMask);
        }
        size_ = 0;
    }

    bool IsEmpty() const { return size_ == 0; }
    size_t Size() const { return size_; }

private:
    struct Entry {
        MSRegionInfo* info;
        uint64_t wordMask;
    };

    Entry entries_[Capacity];
    size_t size_ = 0;
};

struct MSMarkPublicationContext {
    MSPendingMarkRegionBatch<8> pending;
    bool readyPublished = false;
};

// An allocation-free coalescing cache for mark bitmap updates. The batch
// deliberately stores only the stable bitmap coordinate. ScanClass,
// collect-set membership and all plane addresses are derived at flush time.
// Different owners choose only the cache geometry; add/flush semantics stay
// in MarkSweepCollector's shared implementation.
template<size_t SetCount>
class MSMarkWordBatchStorage {
public:
    static constexpr size_t kSetCount = SetCount;
    static constexpr size_t kWays = 4;
    static constexpr size_t kCapacity = kSetCount * kWays;
    static_assert(kCapacity <= 32,
                  "MarkWordBatch occupancy mask supports at most 32 entries");

    MSMarkWordBatchStorage() = default;
    MSMarkWordBatchStorage(const MSMarkWordBatchStorage&) = delete;
    MSMarkWordBatchStorage& operator=(const MSMarkWordBatchStorage&) = delete;

    ~MSMarkWordBatchStorage()
    {
        CHECK_DETAIL(IsEmpty(),
                     "[MS-GC] destroying non-empty MarkWordBatch entries=%zu",
                     size_);
        CHECK_DETAIL(
            publication_.pending.IsEmpty() &&
            !publication_.readyPublished,
            "[MS-GC] destroying MarkWordBatch with pending publication");
    }

    bool HasMarkEpoch() const { return markEpochInitialized_; }

    // Owner-specific diagnostics. Runtime barrier adapters override these;
    // GC-owned batches intentionally keep them as no-ops.
    void RecordResolveRejected() const {}

    uint32_t GetMarkEpoch() const
    {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        CHECK_DETAIL(markEpochInitialized_,
                     "[MS-GC] reading uninitialized MarkWordBatch epoch");
#endif
        return markEpoch_;
    }

    void SetMarkEpoch(uint32_t markEpoch)
    {
        CHECK_DETAIL(!markEpochInitialized_ || markEpoch_ == markEpoch,
                     "[MS-GC] MarkWordBatch crossed mark epochs old=%u new=%u",
                     markEpoch_, markEpoch);
        markEpoch_ = markEpoch;
        markEpochInitialized_ = true;
    }

    bool HasBitmapPlaneBases() const { return bitmapPlaneBasesInitialized_; }

    void SetBitmapPlaneBases(uintptr_t allocPlaneBase, uintptr_t markPlaneBase)
    {
        CHECK_DETAIL(
            !bitmapPlaneBasesInitialized_ ||
            (allocPlaneBase_ == allocPlaneBase && markPlaneBase_ == markPlaneBase),
            "[MS-GC] MarkWordBatch bitmap planes changed");
        allocPlaneBase_ = allocPlaneBase;
        markPlaneBase_ = markPlaneBase;
        bitmapPlaneBasesInitialized_ = true;
    }

    uint64_t* GetAllocBitmap(uint32_t regionIdx) const
    {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        CHECK_DETAIL(bitmapPlaneBasesInitialized_,
                     "[MS-GC] reading uninitialized MarkWordBatch alloc plane");
#endif
        return reinterpret_cast<uint64_t*>(
            allocPlaneBase_ +
            static_cast<uintptr_t>(regionIdx) * MSBitmapArena::kPlaneStrideBytes);
    }

    uint64_t* GetMarkBitmap(uint32_t regionIdx) const
    {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        CHECK_DETAIL(bitmapPlaneBasesInitialized_,
                     "[MS-GC] reading uninitialized MarkWordBatch mark plane");
#endif
        return reinterpret_cast<uint64_t*>(
            markPlaneBase_ +
            static_cast<uintptr_t>(regionIdx) * MSBitmapArena::kPlaneStrideBytes);
    }

    template<typename FlushEntry>
    void Add(uint32_t regionIdx, size_t wordIdx, uint64_t mask,
             FlushEntry&& flushEntry)
    {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        CHECK_DETAIL(
            regionIdx != MSRegionInfo::INVALID_REGION_IDX &&
            wordIdx < MSBitmapArena::kMaxBitmapWords && mask != 0,
            "[MS-GC] invalid MarkWordBatch coordinate region=%u word=%zu mask=0x%llx",
            regionIdx, wordIdx, static_cast<unsigned long long>(mask));
#endif

        uint64_t wordKey = MakeWordKey(regionIdx, wordIdx);
        uint8_t last = lastEntry_;
        if (last < kCapacity && IsOccupied(last) &&
            entries_[last].wordKey == wordKey) {
            entries_[last].mask |= mask;
            return;
        }

        size_t set = HashSet(regionIdx, wordIdx);
        size_t first = set * kWays;
        uint32_t allWays = (UINT32_C(1) << kWays) - 1;
        uint32_t occupiedSet = (occupiedMask_ >> first) & allWays;
        uint32_t occupiedWays = occupiedSet;
        if (last >= first && last < first + kWays) {
            occupiedWays &= ~(UINT32_C(1) << (last - first));
        }
        while (occupiedWays != 0) {
            size_t way = static_cast<size_t>(__builtin_ctz(occupiedWays));
            size_t i = first + way;
            MSMarkWordBatchEntry& entry = entries_[i];
            if (entry.wordKey == wordKey) {
                entry.mask |= mask;
                lastEntry_ = static_cast<uint8_t>(i);
                return;
            }
            occupiedWays &= occupiedWays - 1;
        }

        if (occupiedSet != allWays) {
            size_t emptyWay = static_cast<size_t>(
                __builtin_ctz((~occupiedSet) & allWays));
            size_t emptyIdx = first + emptyWay;
            Fill(entries_[emptyIdx], wordKey, mask);
            occupiedMask_ |= EntryBit(emptyIdx);
            ++size_;
            lastEntry_ = static_cast<uint8_t>(emptyIdx);
            return;
        }

        size_t victimShift = set * 2;
        size_t victimWay = static_cast<size_t>(
            (nextVictimWayBits_ >> victimShift) & UINT32_C(3));
        size_t victimIdx = first + victimWay;
        MSMarkWordBatchEntry& victim = entries_[victimIdx];
        flushEntry(victim);
        Fill(victim, wordKey, mask);
        uint32_t victimMask = UINT32_C(3) << victimShift;
        nextVictimWayBits_ =
            (nextVictimWayBits_ & ~victimMask) |
            (static_cast<uint32_t>((victimWay + 1) & 3) << victimShift);
        lastEntry_ = static_cast<uint8_t>(victimIdx);
    }

    template<typename FlushEntry>
    void FlushAll(FlushEntry&& flushEntry)
    {
        uint32_t occupied = occupiedMask_;
        while (occupied != 0) {
            size_t i = static_cast<size_t>(__builtin_ctz(occupied));
            occupied &= occupied - 1;
            flushEntry(entries_[i]);
        }
        occupiedMask_ = 0;
        size_ = 0;
        lastEntry_ = kInvalidEntry;
    }

    bool IsEmpty() const { return size_ == 0; }
    size_t Size() const { return size_; }
    MSMarkPublicationContext& Publication() { return publication_; }

private:
    static constexpr uint32_t EntryBit(size_t index)
    {
        return UINT32_C(1) << index;
    }

    static constexpr uint8_t kInvalidEntry = UINT8_C(0xff);

    static uint64_t MakeWordKey(uint32_t regionIdx, size_t wordIdx)
    {
        return (static_cast<uint64_t>(regionIdx) << 6) |
            static_cast<uint64_t>(wordIdx);
    }

    bool IsOccupied(size_t index) const
    {
        return (occupiedMask_ & EntryBit(index)) != 0;
    }

    static size_t HashSet(uint32_t regionIdx, size_t wordIdx)
    {
        // Both Region indices and bitmap-word indices are often sequential.
        // Odd multiplicative constants distribute their low bits while keeping
        // the set selection cheap on the supported 64-bit targets.
        uint32_t hash =
            regionIdx * UINT32_C(2654435761) +
            static_cast<uint32_t>(wordIdx) * UINT32_C(2246822519);
        return static_cast<size_t>(hash) & (kSetCount - 1);
    }

    static void Fill(MSMarkWordBatchEntry& entry, uint64_t wordKey,
                     uint64_t mask)
    {
        entry.wordKey = wordKey;
        entry.mask = mask;
    }

    MSMarkWordBatchEntry entries_[kCapacity];
    uint32_t occupiedMask_ = 0;
    size_t size_ = 0;
    uint32_t nextVictimWayBits_ = 0;
    uint8_t lastEntry_ = kInvalidEntry;
    uint32_t markEpoch_ = MSRegionInfo::NO_COLLECT_EPOCH;
    uintptr_t allocPlaneBase_ = 0;
    uintptr_t markPlaneBase_ = 0;
    bool markEpochInitialized_ = false;
    bool bitmapPlaneBasesInitialized_ = false;
    MSMarkPublicationContext publication_;
};

// All GC-owned producers use the same 32-entry local storage. The compiler-
// visible write-barrier POD also has 32 entries, but retains its ABI-specific
// layout and is exposed to the shared collector logic through a storage view.
using MSMarkWordBatch = MSMarkWordBatchStorage<8>;

static_assert(sizeof(MSMarkWordBatchEntry) == 16,
              "MarkWordBatch entry must stay compact");
static_assert((MSMarkWordBatch::kSetCount & (MSMarkWordBatch::kSetCount - 1)) == 0,
              "MarkWordBatch set count must stay a power of two");

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_MARK_BATCH_H
