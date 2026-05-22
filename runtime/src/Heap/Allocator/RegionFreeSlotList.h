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
#include "Heap/Allocator/RegionFreeSlot.h"

namespace MapleRuntime {

class RegionInfo;

struct FreeSlotNode {
    static constexpr uintptr_t MAGIC_VALUE = 0xABCu;
    static constexpr size_t OFFSET_LEN = 32;
    static constexpr size_t SIZE_LEN = 20;
    static constexpr size_t MAGIC_LEN = 12;

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
};

class RegionFreeSlotList {
public:
    static constexpr size_t NUM_SLOTS = RegionFreeSlot::NUM_SLOTS;

    RegionFreeSlotList() : regionStart_(0), heads_{}, maxSlotIndex_(NUM_SLOTS) {}

    void Init(uintptr_t regionStart)
    {
        regionStart_ = regionStart;
        for (size_t i = 0; i < NUM_SLOTS; i++) {
            heads_[i] = INVALID_OFFSET;
        }
        maxSlotIndex_ = NUM_SLOTS;
    }

    size_t GetMaxAvailableSlotIndex() const { return maxSlotIndex_; }

    size_t GetMaxAvailableSize() const
    {
        if (maxSlotIndex_ >= NUM_SLOTS) {
            return 0;
        }
        return RegionFreeSlot::SLOT_SIZES[maxSlotIndex_];
    }

    void AddSlot(uint32_t offset, uint32_t size)
    {
        size_t slotIdx = RegionFreeSlot::GetAlignedDownSlotIndex(size);
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
        size_t slotIdx = RegionFreeSlot::GetAlignedUpSlotIndex(requestedSize);
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
