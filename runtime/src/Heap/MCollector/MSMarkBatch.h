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
    uint32_t regionIdx = MSRegionInfo::INVALID_REGION_IDX;
    uint8_t wordIdx = 0;
    uint8_t reserved[3] = {};
    uint64_t mask = 0;

    bool IsEmpty() const
    {
        return regionIdx == MSRegionInfo::INVALID_REGION_IDX;
    }

    void Reset()
    {
        regionIdx = MSRegionInfo::INVALID_REGION_IDX;
        wordIdx = 0;
        mask = 0;
    }
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
    static constexpr size_t kWays = 2;
    static constexpr size_t kCapacity = kSetCount * kWays;

    MSMarkWordBatchStorage() = default;
    MSMarkWordBatchStorage(const MSMarkWordBatchStorage&) = delete;
    MSMarkWordBatchStorage& operator=(const MSMarkWordBatchStorage&) = delete;

    ~MSMarkWordBatchStorage()
    {
        CHECK_DETAIL(IsEmpty(),
                     "[MS-GC] destroying non-empty MarkWordBatch entries=%zu",
                     size_);
    }

    bool HasMarkEpoch() const { return markEpochInitialized_; }

    // Owner-specific diagnostics. Runtime barrier adapters override these;
    // GC-owned batches intentionally keep them as no-ops.
    void RecordResolveRejected() const {}
    void RecordLargeRef() const {}

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

        size_t set = HashSet(regionIdx, wordIdx);
        size_t first = set * kWays;
        size_t firstEmpty = kCapacity;
        for (size_t way = 0; way < kWays; ++way) {
            size_t i = first + way;
            MSMarkWordBatchEntry& entry = entries_[i];
            if (entry.regionIdx == regionIdx && entry.wordIdx == wordIdx) {
                entry.mask |= mask;
                return;
            }
            if (firstEmpty == kCapacity && entry.IsEmpty()) {
                firstEmpty = i;
            }
        }

        if (firstEmpty != kCapacity) {
            Fill(entries_[firstEmpty], regionIdx, wordIdx, mask);
            ++size_;
            return;
        }

        size_t victimIdx = first + nextVictimWay_[set];
        MSMarkWordBatchEntry& victim = entries_[victimIdx];
        flushEntry(victim);
        Fill(victim, regionIdx, wordIdx, mask);
        nextVictimWay_[set] =
            static_cast<uint8_t>((nextVictimWay_[set] + 1) % kWays);
    }

    template<typename FlushEntry>
    void FlushAll(FlushEntry&& flushEntry)
    {
        for (size_t i = 0; i < kCapacity; ++i) {
            MSMarkWordBatchEntry& entry = entries_[i];
            if (entry.IsEmpty()) {
                continue;
            }
            flushEntry(entry);
            entry.Reset();
        }
        size_ = 0;
        for (size_t i = 0; i < kSetCount; ++i) {
            nextVictimWay_[i] = 0;
        }
    }

    bool IsEmpty() const { return size_ == 0; }
    size_t Size() const { return size_; }

private:
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

    static void Fill(MSMarkWordBatchEntry& entry, uint32_t regionIdx,
                     size_t wordIdx, uint64_t mask)
    {
        entry.regionIdx = regionIdx;
        entry.wordIdx = static_cast<uint8_t>(wordIdx);
        entry.mask = mask;
    }

    MSMarkWordBatchEntry entries_[kCapacity];
    size_t size_ = 0;
    uint8_t nextVictimWay_[kSetCount] = {};
    uint32_t markEpoch_ = MSRegionInfo::NO_COLLECT_EPOCH;
    uintptr_t allocPlaneBase_ = 0;
    uintptr_t markPlaneBase_ = 0;
    bool markEpochInitialized_ = false;
    bool bitmapPlaneBasesInitialized_ = false;
};

// The default batch is used by ordinary GC marking and the no-owner-buffer
// write-barrier fallback. Root publication keeps a separate owner alias so its
// geometry can be tuned independently without changing shared add/flush code.
using MSMarkWordBatch = MSMarkWordBatchStorage<64>;
using MSRootMarkWordBatch = MSMarkWordBatchStorage<64>;

static_assert(sizeof(MSMarkWordBatchEntry) == 16,
              "MarkWordBatch entry must stay compact");
static_assert((MSMarkWordBatch::kSetCount & (MSMarkWordBatch::kSetCount - 1)) == 0,
              "MarkWordBatch set count must stay a power of two");
static_assert((MSRootMarkWordBatch::kSetCount &
               (MSRootMarkWordBatch::kSetCount - 1)) == 0,
              "RootMarkWordBatch set count must stay a power of two");

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_MARK_BATCH_H
