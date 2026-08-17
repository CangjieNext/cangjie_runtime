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

// Maximum small-object size. Objects strictly larger than this go to the
// large-object path; exactly 16KB remains in class 35 (two slots per Region).
static constexpr size_t kLargeObjectThreshold = 16384;

static_assert(kNumSizeClasses == 36, "compiler/runtime MSGC size-class count changed");
static_assert(kSizeClasses[kNumSizeClasses - 1] == kLargeObjectThreshold,
              "the final MSGC size class must match the small-object boundary");

struct MSSizeClassDesc {
    uint64_t validTailMask;
    uint32_t divMagic;
    uint16_t slotSize;
    uint16_t totalSlots;
    uint8_t bitmapWords;
    uint8_t reserved[7];
};

constexpr MSSizeClassDesc MakeMSSizeClassDesc(size_t slotSize)
{
    const size_t totalSlots = MS_REGION_SIZE / slotSize;
    const size_t tailBits = totalSlots % 64;
    return MSSizeClassDesc {
        tailBits == 0 ? ~0ULL : ((1ULL << tailBits) - 1),
        static_cast<uint32_t>((0x100000000ULL + slotSize - 1) / slotSize),
        static_cast<uint16_t>(slotSize),
        static_cast<uint16_t>(totalSlots),
        static_cast<uint8_t>((totalSlots + 63) / 64),
        { 0, 0, 0, 0, 0, 0, 0 },
    };
}

#define MRT_MS_SIZE_CLASS_DESC_ENTRY(slotSize) MakeMSSizeClassDesc(slotSize)
static constexpr MSSizeClassDesc kMSSizeClassDescs[kNumSizeClasses] = {
    MRT_MS_SIZE_CLASS_DESC_ENTRY(8),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(16),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(24),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(32),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(48),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(64),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(80),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(96),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(112),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(128),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(160),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(192),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(224),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(256),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(320),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(384),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(448),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(512),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(640),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(768),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(896),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(1024),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(1280),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(1536),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(1792),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(2048),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(2560),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(3072),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(3584),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(4096),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(5120),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(6144),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(8192),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(10240),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(12288),
    MRT_MS_SIZE_CLASS_DESC_ENTRY(16384),
};
#undef MRT_MS_SIZE_CLASS_DESC_ENTRY

static_assert(sizeof(MSSizeClassDesc) == 24, "MSSizeClassDesc layout changed");
static_assert(kMSSizeClassDescs[0].totalSlots == 4096, "8-byte class slot count");
static_assert(kMSSizeClassDescs[0].bitmapWords == 64, "8-byte class bitmap words");
static_assert(kMSSizeClassDescs[0].validTailMask == ~0ULL, "8-byte class tail mask");
static_assert(kMSSizeClassDescs[2].totalSlots == 1365, "24-byte class slot count");
static_assert(kMSSizeClassDescs[2].validTailMask == ((1ULL << 21) - 1), "24-byte class tail mask");
static_assert(kMSSizeClassDescs[kNumSizeClasses - 1].totalSlots == 2, "16KB class slot count");

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

    static constexpr bool IsLargeObjectSize(size_t size)
    {
        return size > kLargeObjectThreshold;
    }

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
        return kMSSizeClassDescs[cls].slotSize;
    }

    static inline const MSSizeClassDesc& ClassToDesc(uint8_t cls)
    {
        return kMSSizeClassDescs[cls];
    }

    static inline uint16_t ClassToTotalSlots(uint8_t cls)
    {
        return ClassToDesc(cls).totalSlots;
    }

    static inline uint8_t ClassToBitmapWords(uint8_t cls)
    {
        return ClassToDesc(cls).bitmapWords;
    }

    static inline uint64_t ClassToValidTailMask(uint8_t cls)
    {
        return ClassToDesc(cls).validTailMask;
    }

    // slotIdx = offset / ClassToSize(cls), division-free.
    static inline size_t OffsetToSlotIndex(uint8_t cls, size_t offset)
    {
        return static_cast<size_t>(
            (static_cast<uint64_t>(offset) * ClassToDesc(cls).divMagic) >> 32);
    }

private:
    // Lookup tables (populated in Init())
    static uint8_t smallLookup_[kSmallLookupEntries];   // size/8 -> class (size <= 1024)
    static uint8_t largeLookup_[kLargeLookupEntries];   // (size-1024)/256 -> class (size > 1024)
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_SIZE_CLASS_H
