// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSSizeClass.h"

#include "Base/Log.h"

namespace MapleRuntime {

// Static member definitions
uint8_t SizeClassInfo::smallLookup_[kSmallLookupEntries] = { 0 };
uint8_t SizeClassInfo::largeLookup_[kLargeLookupEntries] = { 0 };
size_t  SizeClassInfo::slotsPerRegion_[kNumSizeClasses] = { 0 };

void SizeClassInfo::Init()
{
    // 1. Populate slotsPerRegion_
    for (size_t cls = 0; cls < kNumSizeClasses; cls++) {
        slotsPerRegion_[cls] = MS_REGION_SIZE / kSizeClasses[cls];
    }

    // 2. Build small lookup table: for size in [1, 1024], index = ceil(size/8)
    //    Each entry stores the class whose ClassToSize >= size
    size_t cls = 0;
    for (size_t idx = 0; idx < kSmallLookupEntries; idx++) {
        size_t size = idx * kSmallGranularity;
        if (size == 0) {
            size = 1;  // size=0 should map to class 0 (8B)
        }
        // Advance cls until kSizeClasses[cls] >= size
        while (cls < kNumSizeClasses - 1 && kSizeClasses[cls] < size) {
            cls++;
        }
        smallLookup_[idx] = static_cast<uint8_t>(cls);
    }

    // 3. Build large lookup table: for size in (1024, 16384], index = ceil((size-1024)/256)
    cls = 0;
    // Find the first class >= 1024
    while (cls < kNumSizeClasses && kSizeClasses[cls] < kSmallLookupBound) {
        cls++;
    }
    for (size_t idx = 0; idx < kLargeLookupEntries; idx++) {
        size_t size = kSmallLookupBound + idx * kLargeGranularity;
        if (idx == 0) {
            size = kSmallLookupBound + 1;  // first entry: size just above 1024
        }
        // Advance cls until kSizeClasses[cls] >= size
        while (cls < kNumSizeClasses - 1 && kSizeClasses[cls] < size) {
            cls++;
        }
        largeLookup_[idx] = static_cast<uint8_t>(cls);
    }

    // 4. Validate: SizeToClass must satisfy ClassToSize(SizeToClass(n)) >= n for all n in [1, kLargeObjectThreshold]
#if defined(MRT_DEBUG) && MRT_DEBUG
    for (size_t n = 1; n <= kLargeObjectThreshold; n++) {
        uint8_t c = SizeToClass(n);
        size_t slotSize = ClassToSize(c);
        if (slotSize < n) {
            LOG(RTLOG_FATAL, "[MS-GC] SizeClass validation failed: SizeToClass(%zu)=%u, ClassToSize=%zu < %zu",
                n, c, slotSize, n);
        }
    }
    LOG(RTLOG_INFO, "[MS-GC] SizeClassInfo::Init() completed: %zu classes, Region=%zuKB",
        kNumSizeClasses, MS_REGION_SIZE / 1024);
#endif
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
