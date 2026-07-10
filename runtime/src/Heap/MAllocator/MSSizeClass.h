// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_SIZE_CLASS_H
#define MRT_MS_SIZE_CLASS_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>

#include "Base/Globals.h"

namespace MapleRuntime {

// ============================================================================
// Size Class Design for Mark-Sweep GC
// ============================================================================
//
// Each Region (MS_REGION_SIZE bytes) serves exactly one size class.
// All slots within the Region have the same size = ClassToSize(cls).
//
// Design principles:
//   - 8-byte alignment (matches Allocator::ALLOC_ALIGN)
//   - ~12.5-25% growth between adjacent classes
//   - 36 classes covering 8B..16384B
//   - Objects > kLargeObjectThreshold go to the large-region path
//
// Reference: Go runtime msize.go (67 classes), adapted for Cangjie.
// ============================================================================

// MS mode Region size: 32KB fixed.
// On macOS ARM64 (16KB pages) this is 2 pages; on Linux x86_64 (4KB pages) this is 8 pages.
static constexpr size_t MS_REGION_SIZE = 32 * 1024;  // 32KB

// Number of size classes
static constexpr size_t kNumSizeClasses = 36;

// Size class table: index -> slot size in bytes
// Index 0 = 8B (minimum object size, 8-byte aligned)
// Index 35 = 16384B (maximum small-object, = MS_REGION_SIZE / 2)
static constexpr size_t kSizeClasses[kNumSizeClasses] = {
    // Small objects (8B ~ 128B), step 8~16B
    8,      // class 0
    16,     // class 1
    24,     // class 2
    32,     // class 3
    48,     // class 4
    64,     // class 5
    80,     // class 6
    96,     // class 7
    112,    // class 8
    128,    // class 9

    // Medium-small objects (160B ~ 512B), step 32~64B
    160,    // class 10
    192,    // class 11
    224,    // class 12
    256,    // class 13
    320,    // class 14
    384,    // class 15
    448,    // class 16
    512,    // class 17

    // Medium objects (640B ~ 2048B), step 128~256B
    640,    // class 18
    768,    // class 19
    896,    // class 20
    1024,   // class 21
    1280,   // class 22
    1536,   // class 23
    1792,   // class 24
    2048,   // class 25

    // Medium-large objects (2560B ~ 8192B), step 512~2048B
    2560,   // class 26
    3072,   // class 27
    3584,   // class 28
    4096,   // class 29
    5120,   // class 30
    6144,   // class 31
    8192,   // class 32

    // Large-small objects (10240B ~ 16384B)
    10240,  // class 33
    12288,  // class 34
    16384,  // class 35
};

// Large object threshold: objects > this go to AllocLarge path (Tier 1 or Tier 2)
// = MS_REGION_SIZE / 2 = 16KB
// Rationale: a 16KB object in a 32KB Region can have at most 2 slots,
// making bitmap allocation inefficient.
static constexpr size_t kLargeObjectThreshold = 16384;

// ============================================================================
// SizeToClass lookup acceleration
// ============================================================================
// For size <= 1024: direct table indexed by size/8  (128 entries)
// For 1024 < size <= 16384: table indexed by (size-1024+255)/256  (60 entries)
static constexpr size_t kSmallLookupBound = 1024;
static constexpr size_t kSmallGranularity = 8;      // alignment granularity for small
static constexpr size_t kLargeGranularity = 256;     // alignment granularity for large lookup
static constexpr size_t kSmallLookupEntries = kSmallLookupBound / kSmallGranularity + 1;  // 129
static constexpr size_t kLargeLookupEntries = (kLargeObjectThreshold - kSmallLookupBound + kLargeGranularity - 1)
                                              / kLargeGranularity + 1;  // ~61

class SizeClassInfo {
public:
    // Must be called once at runtime startup (under MRT_USE_MARK_SWEEP).
    static void Init();

    // O(1) size -> class lookup.
    // Precondition: 0 < size <= kLargeObjectThreshold
    static inline uint8_t SizeToClass(size_t size)
    {
        if (size == 0) {
            size = kSmallGranularity;  // minimum allocation = 8B
        }
        if (size <= kSmallLookupBound) {
            size_t idx = (size + kSmallGranularity - 1) / kSmallGranularity;  // ceil(size/8)
            return smallLookup_[idx];
        }
        // 1024 < size <= 16384
        size_t idx = (size - kSmallLookupBound + kLargeGranularity - 1) / kLargeGranularity;
        return largeLookup_[idx];
    }

    // O(1) class -> slot size lookup.
    static inline size_t ClassToSize(uint8_t cls)
    {
        return kSizeClasses[cls];
    }

    // Number of slots that fit in one Region of the given class.
    static inline size_t SlotsPerRegion(uint8_t cls)
    {
        return slotsPerRegion_[cls];
    }

    // Number of uint64_t words needed for the bitmap of the given class.
    static inline size_t BitmapWordsPerRegion(uint8_t cls)
    {
        return (slotsPerRegion_[cls] + 63) / 64;
    }

    // Reciprocal magic for dividing a region offset by ClassToSize(cls) without
    // a hardware division (Route-B / R-2): given magic = ceil(2^32 / slotSize),
    //   slotIdx = (uint64_t)offset * magic >> 32
    // is exact for offset < MS_REGION_SIZE (2^15) and slotSize in [8, 16384].
    // (Error term offset*s/(slotSize*2^32) < 2^-17 < 1/slotSize, so it never
    //  crosses an integer boundary. See Step37 §5.6.)
    static inline uint32_t ClassToDivMagic(uint8_t cls)
    {
        return slotDivMagic_[cls];
    }

    // slotIdx = offset / ClassToSize(cls), division-free.
    static inline size_t OffsetToSlotIndex(uint8_t cls, size_t offset)
    {
        return static_cast<size_t>(
            (static_cast<uint64_t>(offset) * slotDivMagic_[cls]) >> 32);
    }

private:
    // Lookup tables (populated in Init())
    static uint8_t smallLookup_[kSmallLookupEntries];   // size/8 -> class (size <= 1024)
    static uint8_t largeLookup_[kLargeLookupEntries];   // (size-1024)/256 -> class (size > 1024)
    static size_t  slotsPerRegion_[kNumSizeClasses];    // class -> slots per Region
    static uint32_t slotDivMagic_[kNumSizeClasses];     // class -> ceil(2^32/slotSize)
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_SIZE_CLASS_H
