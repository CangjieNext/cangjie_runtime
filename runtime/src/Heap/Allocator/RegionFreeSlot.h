// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REGION_FREE_SLOT_H
#define MRT_REGION_FREE_SLOT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include "Base/Globals.h"

namespace MapleRuntime {

class BaseObject;
class RegionFreeSlot {
public:
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
    static constexpr size_t POINTER_LEN = 32;
    static constexpr size_t SIZE_LEN = 20;
    static constexpr size_t MAGIC_LEN = 12;
    struct TaggedPointer {
        uintptr_t next : POINTER_LEN;
        uintptr_t size : SIZE_LEN;
        uintptr_t magic : MAGIC_LEN;
    } taggedPointer;
};

} // namespace MapleRuntime

#endif // MRT_REGION_FREE_SLOT_H