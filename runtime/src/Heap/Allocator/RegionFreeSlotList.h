// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGION_FREE_SLOT_LIST_H
#define MRT_REGION_FREE_SLOT_LIST_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "Base/Globals.h"
#include "Base/Log.h"

namespace MapleRuntime {

class RegionInfo;

struct FreeSlotNode {
    static constexpr uintptr_t MAGIC_VALUE = 0xABCu;
    static constexpr size_t OFFSET_LEN = 32;
    static constexpr size_t SIZE_LEN = 20;
    static constexpr size_t MAGIC_LEN = 12;

    static FreeSlotNode* Cast(uintptr_t addr)
    {
        return reinterpret_cast<FreeSlotNode*>(addr);
    }

    uintptr_t offset : OFFSET_LEN;
    uintptr_t size : SIZE_LEN;
    uintptr_t magic : MAGIC_LEN;

    bool IsValidSlot() const { return magic == MAGIC_VALUE; }

    static FreeSlotNode MakeSlot(uint32_t off, uint32_t sz)
    {
        FreeSlotNode node;
        node.offset = off;
        node.size = sz;
        node.magic = MAGIC_VALUE;
        return node;
    }

    // --- Slot size classification (merged from RegionFreeSlot) ---

    static constexpr size_t SLOT_SIZES[] = {
        8, 16, 24, 32, 48, 64, 96, 128,
        192, 256, 384, 512,
        768, KB, 2 * KB, 4 * KB,
        8 * KB, 12 * KB, 16 * KB, 20 * KB, 24 * KB, 28 * KB, 32 * KB, 64 * KB, 128 * KB, 256 * KB, 512 * KB, 1 * MB, 2 * MB
    };
    static constexpr size_t NUM_SLOTS = sizeof(SLOT_SIZES) / sizeof(SLOT_SIZES[0]);

    static constexpr size_t SMALL_SIZE_THRESHOLD = 256;
    static constexpr size_t KB_4 = 4 * KB;
    static constexpr size_t MEDIUM_SLOT_START = 10;
    static constexpr size_t MEDIUM_SLOT_END = 15;
    static constexpr size_t KB_THRESHOLD = SLOT_SIZES[MEDIUM_SLOT_END];
    static constexpr size_t FIRST_4KB_INDEX = MEDIUM_SLOT_END + 1;
    static constexpr size_t KB_32 = SLOT_SIZES[NUM_SLOTS - 2];
    static constexpr size_t KB_64 = SLOT_SIZES[NUM_SLOTS - 1];
    static constexpr size_t MAX_SIZE = KB_64;

    static void InitSizeToSlotMap()
    {
        size_t slotIdx = 0;
        for (size_t size = 0; size <= SMALL_SIZE_THRESHOLD; size++) {
            while (slotIdx < NUM_SLOTS - 1 && SLOT_SIZES[slotIdx] < size) {
                slotIdx++;
            }
            smallSizeToSlotIdx[size] = static_cast<uint8_t>(slotIdx);
        }
    }

    static size_t GetAlignedUpSlotIndex(size_t size)
    {
        if (size == 0) {
            return 0;
        }
        if (size > MAX_SIZE) {
            return NUM_SLOTS;
        }
        if (size <= SMALL_SIZE_THRESHOLD) {
            return smallSizeToSlotIdx[size];
        }
        if (size <= KB_THRESHOLD) {
            return GetMediumSlotIndex(size);
        }
        return GetLargeSlotIndex(size);
    }

    static size_t GetSlotSize(size_t size)
    {
        size_t idx = GetAlignedUpSlotIndex(size);
        if (idx >= NUM_SLOTS) {
            return 0;
        }
        return SLOT_SIZES[idx];
    }

    static size_t GetAlignedDownSlotIndex(size_t size)
    {
        if (size == 0) {
            return 0;
        }
        if (size < SLOT_SIZES[0]) {
            return 0;
        }
        if (size >= MAX_SIZE) {
            return NUM_SLOTS - 1;
        }
        for (size_t i = NUM_SLOTS - 1; i > 0; i--) {
            if (SLOT_SIZES[i] <= size) {
                return i;
            }
        }
        return 0;
    }

private:
    static size_t GetMediumSlotIndex(size_t size)
    {
        for (size_t i = MEDIUM_SLOT_START; i <= MEDIUM_SLOT_END; i++) {
            if (size <= SLOT_SIZES[i]) {
                return i;
            }
        }
        return NUM_SLOTS;
    }

    static size_t GetLargeSlotIndex(size_t size)
    {
        if (size <= KB_32) {
            size_t unitIdx = (size + KB_4 - 1) / KB_4;
            return FIRST_4KB_INDEX + (unitIdx - 2);
        }
        return NUM_SLOTS - 1;
    }

    static std::array<uint8_t, SMALL_SIZE_THRESHOLD + 1> smallSizeToSlotIdx;
};

class RegionFreeSlotList {
public:
    static constexpr size_t NUM_SLOTS = FreeSlotNode::NUM_SLOTS;
    static constexpr size_t MIN_SLOT_SIZE = sizeof(FreeSlotNode) + sizeof(uintptr_t);

    RegionFreeSlotList() : regionStart_(0), heads_{}, maxSlotIndex_(NUM_SLOTS), totalSlotSize_(0) {}

    void Init(uintptr_t regionStart)
    {
        regionStart_ = regionStart;
        for (size_t i = 0; i < NUM_SLOTS; i++) {
            heads_[i] = INVALID_OFFSET;
        }
        maxSlotIndex_ = NUM_SLOTS;
        totalSlotSize_ = 0;
    }

    size_t GetMaxAvailableSlotIndex() const { return maxSlotIndex_; }

    size_t GetTotalSlotSize() const { return totalSlotSize_; }

    size_t GetMaxAvailableSize() const
    {
        if (maxSlotIndex_ >= NUM_SLOTS) {
            return 0;
        }
        return FreeSlotNode::SLOT_SIZES[maxSlotIndex_];
    }

    void AddSlot(uint32_t offset, uint32_t size)
    {
        DCHECK(size >= MIN_SLOT_SIZE);
        totalSlotSize_ += size;
        size_t slotIdx = FreeSlotNode::GetAlignedDownSlotIndex(size);
        if (slotIdx >= NUM_SLOTS) {
            slotIdx = NUM_SLOTS - 1;
        }
        uint32_t next = heads_[slotIdx];
        heads_[slotIdx] = offset;
        WriteSlotNode(offset, FreeSlotNode::MakeSlot(offset, size));
        WriteSlotNext(offset, next);
        if (slotIdx > maxSlotIndex_) {
            maxSlotIndex_ = slotIdx;
        }
    }

    uintptr_t AllocateSlot(size_t requestedSize, uintptr_t regionStart)
    {
        size_t slotIdx = FreeSlotNode::GetAlignedUpSlotIndex(requestedSize);
        if (slotIdx >= NUM_SLOTS) {
            return 0;
        }
        if (maxSlotIndex_ >= NUM_SLOTS || slotIdx > maxSlotIndex_) {
            return 0;
        }

        for (size_t i = slotIdx; i <= maxSlotIndex_; i++) {
            if (heads_[i] != INVALID_OFFSET) {
                uint32_t slotOffset = heads_[i];
                FreeSlotNode node = ReadSlotNode(slotOffset);
                if (!node.IsValidSlot()) {
                    heads_[i] = INVALID_OFFSET;
                    if (i >= maxSlotIndex_) {
                        RecalculateMaxSlotIndex();
                    }
                    continue;
                }

                uint32_t nextOffset = ReadSlotNext(slotOffset);
                heads_[i] = nextOffset;
                if (nextOffset == INVALID_OFFSET && i >= maxSlotIndex_) {
                    RecalculateMaxSlotIndex();
                }

                uint32_t slotSize = node.size;
                totalSlotSize_ -= slotSize;
                if (slotSize > requestedSize) {
                    uint32_t remainderOffset = slotOffset + static_cast<uint32_t>(requestedSize);
                    uint32_t remainderSize = slotSize - static_cast<uint32_t>(requestedSize);
                    AddSlot(remainderOffset, remainderSize);
                }

                return regionStart + slotOffset;
            }
        }
        return 0;
    }

private:
    static constexpr uint32_t INVALID_OFFSET = std::numeric_limits<uint32_t>::max();

    uintptr_t regionStart_;
    std::array<uint32_t, NUM_SLOTS> heads_;
    size_t maxSlotIndex_;
    size_t totalSlotSize_;

    void RecalculateMaxSlotIndex()
    {
        for (size_t i = maxSlotIndex_; i > 0; i--) {
            if (heads_[i] != INVALID_OFFSET) {
                maxSlotIndex_ = i;
                return;
            }
        }
        maxSlotIndex_ = (heads_[0] != INVALID_OFFSET) ? 0 : NUM_SLOTS;
    }

    FreeSlotNode ReadSlotNode(uint32_t offset) const
    {
        FreeSlotNode *node = reinterpret_cast<FreeSlotNode*>(regionStart_ + offset);
        FreeSlotNode copy;
        copy.offset = node->offset;
        copy.size = node->size;
        copy.magic = node->magic;
        return copy;
    }

    void WriteSlotNode(uint32_t offset, FreeSlotNode node)
    {
        FreeSlotNode *ptr = reinterpret_cast<FreeSlotNode*>(regionStart_ + offset);
        ptr->offset = node.offset;
        ptr->size = node.size;
        ptr->magic = node.magic;
    }

    uint32_t ReadSlotNext(uint32_t offset) const
    {
        FreeSlotNode *node = reinterpret_cast<FreeSlotNode*>(regionStart_ + offset);
        uintptr_t raw = *reinterpret_cast<uintptr_t*>(node + 1);
        return static_cast<uint32_t>(raw);
    }

    void WriteSlotNext(uint32_t offset, uint32_t nextOffset)
    {
        FreeSlotNode *node = reinterpret_cast<FreeSlotNode*>(regionStart_ + offset);
        uintptr_t *nextPtr = reinterpret_cast<uintptr_t*>(node + 1);
        *nextPtr = nextOffset;
    }
};

} // namespace MapleRuntime
#endif // MRT_REGION_FREE_SLOT_LIST_H
