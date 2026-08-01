// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_ROOT_DEDUP_TABLE_H
#define MRT_MS_ROOT_DEDUP_TABLE_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace MapleRuntime {

// Reusable open-addressed set for raw root addresses. Reset retains the vector
// allocation and keeps the table at or below 50% load, so root publication
// avoids one heap allocation per candidate and probing always has an empty slot.
class MSRootDedupTable {
public:
    void Reset(size_t expectedRoots)
    {
        lastResetClearedSlotCount_ = usedSlots_.size();
        for (size_t index : usedSlots_) {
            entries_[index] = 0;
        }
        usedSlots_.clear();

        if (expectedRoots == 0) {
            activeCapacity_ = 0;
            mask_ = 0;
            return;
        }

        size_t tableCapacity = 2;
        while (tableCapacity / 2 < expectedRoots) {
            if (tableCapacity > std::numeric_limits<size_t>::max() / 2) {
                tableCapacity = std::numeric_limits<size_t>::max() / 2 + 1;
                break;
            }
            tableCapacity *= 2;
        }
        if (entries_.size() < tableCapacity) {
            entries_.assign(tableCapacity, 0);
        }
        activeCapacity_ = tableCapacity;
        mask_ = tableCapacity - 1;
    }

    bool Insert(uintptr_t address)
    {
        if (address == 0 || activeCapacity_ == 0) {
            return false;
        }

        size_t index = Hash(address) & mask_;
        for (;;) {
            uintptr_t& entry = entries_[index];
            if (entry == address) {
                return false;
            }
            if (entry == 0) {
                entry = address;
                usedSlots_.push_back(index);
                return true;
            }
            index = (index + 1) & mask_;
        }
    }

    size_t Capacity() const { return activeCapacity_; }
    size_t ReservedCapacity() const { return entries_.capacity(); }
    size_t UsedSlotCount() const { return usedSlots_.size(); }
    size_t LastResetClearedSlotCount() const { return lastResetClearedSlotCount_; }

private:
    static size_t Hash(uintptr_t address)
    {
        // Managed object addresses are pointer-aligned. Drop the fixed zero
        // bits, then use an odd multiplicative hash so every power-of-two table
        // retains a full-period mapping of the remaining low address bits.
        return static_cast<size_t>(
            (address >> 3) * UINT64_C(11400714819323198485));
    }

    std::vector<uintptr_t> entries_;
    std::vector<size_t> usedSlots_;
    size_t activeCapacity_ = 0;
    size_t mask_ = 0;
    size_t lastResetClearedSlotCount_ = 0;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_ROOT_DEDUP_TABLE_H
