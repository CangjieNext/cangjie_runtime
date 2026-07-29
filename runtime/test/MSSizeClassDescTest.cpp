// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "Heap/MAllocator/MSSizeClass.h"

using MapleRuntime::MS_REGION_SIZE;
using MapleRuntime::SizeClassInfo;
using MapleRuntime::kNumSizeClasses;

int main()
{
    for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
        const auto& desc = SizeClassInfo::ClassToDesc(cls);
        assert(desc.slotSize == SizeClassInfo::ClassToSize(cls));
        assert(desc.totalSlots == MS_REGION_SIZE / desc.slotSize);
        assert(desc.bitmapWords == (desc.totalSlots + 63) / 64);
        assert(desc.divMagic ==
            static_cast<uint32_t>((0x100000000ULL + desc.slotSize - 1) / desc.slotSize));

        const size_t tailBits = desc.totalSlots % 64;
        const uint64_t expectedTailMask =
            tailBits == 0 ? ~0ULL : ((1ULL << tailBits) - 1);
        assert(desc.validTailMask == expectedTailMask);

        for (size_t slot = 0; slot < desc.totalSlots; ++slot) {
            const size_t offset = slot * desc.slotSize;
            assert(SizeClassInfo::OffsetToSlotIndex(cls, offset) == slot);
        }
    }
    return 0;
}
