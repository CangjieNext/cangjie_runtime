// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_FREE_REGION_MANAGER_H
#define MRT_FREE_REGION_MANAGER_H

#include <algorithm>
#include <atomic>
#include <memory>
#include "CartesianTree.h"
#include "RegionInfo.h"
#include "Common/ScopedObjectAccess.h"

namespace MapleRuntime {
class RegionManager;

// Bitmap cache for fixed-size normal garbage regions. One bit represents one
// normal region, so allocation only needs to find the lowest set bit instead of
// searching for an arbitrary contiguous run of units.
class GarbageBitmap {
public:
    void Init(size_t unitCount, size_t normalUnitCount)
    {
        totalUnits_ = unitCount;
        normalUnitCount_ = normalUnitCount;
        normalRegionCount_ = normalUnitCount_ == 0 ? 0 : unitCount / normalUnitCount_;
        wordCount_ = (normalRegionCount_ + BITS_PER_WORD - 1) / BITS_PER_WORD;
        bits = std::make_unique<uint64_t[]>(wordCount_);
        std::fill(bits.get(), bits.get() + wordCount_, 0ULL);
        summaryWordCount_ = (wordCount_ + BITS_PER_WORD - 1) / BITS_PER_WORD;
        summaryBits = std::make_unique<uint64_t[]>(summaryWordCount_);
        std::fill(summaryBits.get(), summaryBits.get() + summaryWordCount_, 0ULL);
        minSummaryWord_ = summaryWordCount_;
        totalRegionCount_.store(0, std::memory_order_relaxed);
    }

    bool IsNormalRegion(size_t idx, size_t num) const
    {
        return normalUnitCount_ != 0 && num == normalUnitCount_ && idx % normalUnitCount_ == 0 &&
            idx + num <= totalUnits_ && idx / normalUnitCount_ < normalRegionCount_;
    }

    bool Set(size_t idx, size_t num)
    {
        std::lock_guard<std::mutex> lg(mutex_);
        if (!IsNormalRegion(idx, num)) {
            return false;
        }

        size_t bitIdx = idx / normalUnitCount_;
        size_t wordIdx = bitIdx >> LOG2_BITS_PER_WORD;
        uint64_t bitMask = 1ULL << (bitIdx & BIT_INDEX_MASK);
        if (UNLIKELY((bits[wordIdx] & bitMask) != 0)) {
            LOG(RTLOG_FATAL, "duplicate normal garbage region idx %zu units %zu", idx, num);
        }

        bits[wordIdx] |= bitMask;
        SetSummaryBit(wordIdx);
        totalRegionCount_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool TakeUnitsLowAddr(size_t num, size_t& outIdx)
    {
        std::lock_guard<std::mutex> lg(mutex_);
        if (num != normalUnitCount_ || totalRegionCount_.load(std::memory_order_relaxed) == 0) {
            return false;
        }

        for (size_t summaryWordIdx = minSummaryWord_; summaryWordIdx < summaryWordCount_; ++summaryWordIdx) {
            uint64_t summaryWord = summaryBits[summaryWordIdx];
            if (summaryWord == 0) {
                continue;
            }

            size_t wordIdx = summaryWordIdx * BITS_PER_WORD + CountTrailingZeros(summaryWord);
            if (wordIdx >= wordCount_ || bits[wordIdx] == 0) {
                ClearSummaryBit(wordIdx);
                RefreshMinSummaryWord();
                continue;
            }

            uint64_t word = bits[wordIdx];
            size_t bitIdx = wordIdx * BITS_PER_WORD + CountTrailingZeros(word);
            if (UNLIKELY(bitIdx >= normalRegionCount_)) {
                ClearBit(wordIdx, bitIdx);
                RefreshMinSummaryWord();
                continue;
            }

            ClearBit(wordIdx, bitIdx);
            RefreshMinSummaryWord();
            totalRegionCount_.fetch_sub(1, std::memory_order_relaxed);
            outIdx = bitIdx * normalUnitCount_;
            return true;
        }

        RefreshMinSummaryWord();
        return false;
    }

    template<typename Func>
    void DrainAll(Func&& callback)
    {
        std::lock_guard<std::mutex> lg(mutex_);
        size_t runStart = normalRegionCount_;
        size_t runLen = 0;

        auto flushRun = [&]() {
            if (runLen == 0) {
                return;
            }
            callback(runStart * normalUnitCount_, runLen * normalUnitCount_);
            runStart = normalRegionCount_;
            runLen = 0;
        };

        for (size_t wordIdx = 0; wordIdx < wordCount_; ++wordIdx) {
            uint64_t word = bits[wordIdx];
            if (word == 0) {
                flushRun();
                continue;
            }
            while (word != 0) {
                size_t bitIdx = wordIdx * BITS_PER_WORD + CountTrailingZeros(word);
                if (bitIdx >= normalRegionCount_) {
                    break;
                }
                if (runLen == 0) {
                    runStart = bitIdx;
                    runLen = 1;
                } else if (bitIdx == runStart + runLen) {
                    ++runLen;
                } else {
                    flushRun();
                    runStart = bitIdx;
                    runLen = 1;
                }
                word &= word - 1;
            }
        }
        flushRun();

        std::fill(bits.get(), bits.get() + wordCount_, 0ULL);
        std::fill(summaryBits.get(), summaryBits.get() + summaryWordCount_, 0ULL);
        totalRegionCount_.store(0, std::memory_order_relaxed);
        minSummaryWord_ = summaryWordCount_;
    }

    size_t GetTotalCount() const
    {
        return totalRegionCount_.load(std::memory_order_relaxed) * normalUnitCount_;
    }

    size_t GetRegionCount() const { return totalRegionCount_.load(std::memory_order_relaxed); }

    bool IsEmpty() const { return totalRegionCount_.load(std::memory_order_relaxed) == 0; }

private:
    static constexpr size_t BITS_PER_WORD = 64;
    static constexpr size_t LOG2_BITS_PER_WORD = 6;
    static constexpr size_t BIT_INDEX_MASK = BITS_PER_WORD - 1;

    static size_t CountTrailingZeros(uint64_t word)
    {
        MRT_ASSERT(word != 0, "CountTrailingZeros expects non-zero word");
        size_t count = 0;
        while ((word & 1ULL) == 0) {
            word >>= 1;
            ++count;
        }
        return count;
    }

    void SetSummaryBit(size_t wordIdx)
    {
        size_t summaryWordIdx = wordIdx >> LOG2_BITS_PER_WORD;
        uint64_t summaryMask = 1ULL << (wordIdx & BIT_INDEX_MASK);
        summaryBits[summaryWordIdx] |= summaryMask;
        if (summaryWordIdx < minSummaryWord_) {
            minSummaryWord_ = summaryWordIdx;
        }
    }

    void ClearSummaryBit(size_t wordIdx)
    {
        if (wordIdx >= wordCount_) {
            return;
        }
        size_t summaryWordIdx = wordIdx >> LOG2_BITS_PER_WORD;
        uint64_t summaryMask = 1ULL << (wordIdx & BIT_INDEX_MASK);
        summaryBits[summaryWordIdx] &= ~summaryMask;
    }

    void ClearBit(size_t wordIdx, size_t bitIdx)
    {
        if (wordIdx >= wordCount_) {
            return;
        }
        uint64_t bitMask = 1ULL << (bitIdx & BIT_INDEX_MASK);
        bits[wordIdx] &= ~bitMask;
        if (bits[wordIdx] == 0) {
            ClearSummaryBit(wordIdx);
        }
    }

    void RefreshMinSummaryWord()
    {
        while (minSummaryWord_ < summaryWordCount_ && summaryBits[minSummaryWord_] == 0) {
            ++minSummaryWord_;
        }
    }

    std::unique_ptr<uint64_t[]> bits;
    std::unique_ptr<uint64_t[]> summaryBits;
    size_t totalUnits_ = 0;
    size_t normalUnitCount_ = 0;
    size_t normalRegionCount_ = 0;
    size_t wordCount_ = 0;
    size_t summaryWordCount_ = 0;
    size_t minSummaryWord_ = 0;
    std::atomic<size_t> totalRegionCount_{0};
    mutable std::mutex mutex_;
};

// This class is and should be accessed only for region allocation. we do not rely on it to check region status.
class FreeRegionManager {
    using UnitIndex = CartesianTree::Index;
    using UnitCount = CartesianTree::Count;

public:
    explicit FreeRegionManager(RegionManager& manager) : regionManager(manager) {}

    virtual ~FreeRegionManager()
    {
        dirtyUnitTree.Fini();
        releasedUnitTree.Fini();
    }

    void Initialize(UnitCount regionCnt)
    {
        releasedUnitTree.Init(regionCnt);
        dirtyUnitTree.Init(regionCnt);
    }

    RegionInfo* TakeRegion(size_t num, RegionInfo::UnitRole uclass, bool expectPhysicalMem)
    {
        UnitIndex idx = 0;
        bool tryDirtyTree = true;
        bool tryReleasedTree = true;

        // try as hard as we can to take free regions for allocation.
        while (tryDirtyTree || tryReleasedTree) {
            // first try to get a dirty region.
            if (tryDirtyTree && dirtyUnitTreeMutex.try_lock()) {
                bool dirtyOk = dirtyUnitTree.TakeUnitsLowAddr(num, idx);
                if (dirtyOk) {
                    DLOG(REGION, "c-tree %p alloc dirty units[%u+%u, %u) @[0x%zx, 0x%zx), %u dirty-units left",
                        &dirtyUnitTree, idx, num, idx + num, RegionInfo::GetUnitAddress(idx),
                        RegionInfo::GetUnitAddress(idx + num), dirtyUnitTree.GetTotalCount());

                    // it makes sense to slow down allocation by clearing region memory.
                    RegionInfo::ClearUnits(idx, num);
                    RegionInfo* region = RegionInfo::InitRegion(idx, num, uclass);
                    dirtyUnitTreeMutex.unlock();
                    return region;
                }
                tryDirtyTree = false; // once we fail to take units, stop trying.
                dirtyUnitTreeMutex.unlock();
            }

            // then try to get a released region.
            if (tryReleasedTree && releasedUnitTreeMutex.try_lock()) {
                bool releasedOk = releasedUnitTree.TakeUnitsLowAddr(num, idx);
                if (releasedOk) {
#ifdef _WIN64
                    MemMap::CommitMemory(
                        reinterpret_cast<void*>(RegionInfo::GetUnitAddress(idx)), num * RegionInfo::UNIT_SIZE);
#endif
                    DLOG(REGION, "c-tree %p alloc released units[%u+%u, %u) @[0x%zx, 0x%zx), %u released-units left",
                        &releasedUnitTree, idx, num, idx + num, RegionInfo::GetUnitAddress(idx),
                        RegionInfo::GetUnitAddress(idx + num), releasedUnitTree.GetTotalCount());
                    RegionInfo* region = RegionInfo::InitRegion(idx, num, uclass);
                    releasedUnitTreeMutex.unlock();
                    PrehandleReleasedUnit(expectPhysicalMem, idx, num);
                    return region;
                }
                tryReleasedTree = false; // once we fail to take units, stop trying.
                releasedUnitTreeMutex.unlock();
            }
            ScopedEnterSaferegion enterSaferegion(true);
        }

        return nullptr;
    }

    // add units [idx, idx + num)
    void AddGarbageUnits(UnitIndex idx, UnitCount num)
    {
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        if (UNLIKELY(!dirtyUnitTree.MergeInsert(idx, num, true))) {
            LOG(RTLOG_FATAL, "tid %d: failed to add dirty units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }

    void AddReleaseUnits(UnitIndex idx, UnitCount num)
    {
        ScopedEnterSaferegion enterSaferegion(true);
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        if (UNLIKELY(!releasedUnitTree.MergeInsert(idx, num, true))) {
            LOG(RTLOG_FATAL, "tid %d: failed to add release units [%u+%u, %u)", GetTid(), idx, num, idx + num);
        }
    }

    UnitCount GetDirtyUnitCount() const
    {
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        return dirtyUnitTree.GetTotalCount();
    }

    UnitCount GetReleasedUnitCount() const
    {
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        return releasedUnitTree.GetTotalCount();
    }

    // Return the largest contiguous block in released/dirty tree (root node = max-heap top)
    UnitCount GetReleasedMaxBlock() const
    {
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        const auto* r = releasedUnitTree.RootNode();
        return r ? r->GetCount() : 0;
    }
    UnitCount GetDirtyMaxBlock() const
    {
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        const auto* r = dirtyUnitTree.RootNode();
        return r ? r->GetCount() : 0;
    }
    size_t GetReleasedNodeCount() const
    {
        std::lock_guard<std::mutex> lg(releasedUnitTreeMutex);
        return releasedUnitTree.GetNodeCount();
    }
    size_t GetDirtyNodeCount() const
    {
        std::lock_guard<std::mutex> lg(dirtyUnitTreeMutex);
        return dirtyUnitTree.GetNodeCount();
    }

#if defined(MRT_DEBUG)
    void DumpReleasedUnitTree() const { releasedUnitTree.DumpTree("released-unit tree"); }
    void DumpDirtyUnitTree() const { dirtyUnitTree.DumpTree("dirty-unit tree"); }
#endif

    size_t CalculateBytesToRelease() const;
    size_t ReleaseGarbageRegions(size_t targetCachedSize);

private:
    inline void PrehandleReleasedUnit(bool expectPhysicalMem, size_t idx, size_t num) const
    {
        if (expectPhysicalMem) {
            RegionInfo::ClearUnits(idx, num);
        }
    }
    RegionManager& regionManager;

    // physical pages of released units are probably released and they are prepared for allocation.
    mutable std::mutex releasedUnitTreeMutex;
    CartesianTree releasedUnitTree;

    // dirty units are neither cleared nor released, thus must be zeroed explicitly for allocation.
    mutable std::mutex dirtyUnitTreeMutex;
    CartesianTree dirtyUnitTree;
};
} // namespace MapleRuntime
#endif // MRT_FREE_REGION_MANAGER_H
