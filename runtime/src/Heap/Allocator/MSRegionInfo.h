// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_REGION_INFO_H
#define MRT_MS_REGION_INFO_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Base/Globals.h"
#include "SizeClass.h"

namespace MapleRuntime {

// ============================================================================
// MSRegionType: Region lifecycle states for Mark-Sweep GC
// ============================================================================
// Simplified from CopyGC's 15+ RegionType to 5 states.
//
// State transitions:
//   FREE --alloc--> BUMP --full--> FULL --sweep--> PARTIAL --alloc--> FULL
//                                                  PARTIAL --allFree--> FREE
//   FREE --allocLarge--> LARGE --gcSweepDead--> FREE
//
enum class MSRegionType : uint8_t {
    MS_REGION_FREE    = 0,  // Unallocated, in free pool
    MS_REGION_BUMP    = 1,  // First-fill using bump-pointer (allocPtr < regionEnd)
    MS_REGION_PARTIAL = 2,  // Swept, has free slots (freeSlotCount > 0)
    MS_REGION_FULL    = 3,  // No free slots (freeSlotCount == 0)
    MS_REGION_LARGE   = 4,  // Large object Region (single object per Region)
    MS_REGION_SPAN    = 5,  // Secondary MSRegionInfo for unaligned region span; reserved=primaryIdx
};

// Sweep state for lazy sweep protocol
enum class MSSweepState : uint8_t {
    UNSWEPT  = 0,
    SWEEPING = 1,
    SWEPT    = 2,
};

// ============================================================================
// MSRegionInfo: 64-byte per-Region metadata for Mark-Sweep GC
// ============================================================================
// Replaces CopyGC's UnitMetadata (~120 bytes) with a compact structure.
//
// Key design decisions:
//   - allocPtr at offset 0: compiler fast-path ABI constraint
//   - bitmap pointers (not inlined): size varies with sizeClass
//   - 64 bytes = one cache line for optimal iteration
//
// Memory layout (heap-front mirror):
//   MSRegionInfo[i] corresponds to Region[i] at heapStart + i * MS_REGION_SIZE
//   Mirror stored at heapStart - (i+1) * sizeof(MSRegionInfo)
//
struct MSRegionInfo {
    // ========== Allocation core (offset 0, compiler fast-path) ==========
    uintptr_t   allocPtr;           // +0   bump-pointer current position (8B)
    uintptr_t   regionEnd;          // +8   Region end address (8B)

    // ========== Region list management ==========
    int32_t     nextRegionIdx;      // +16  linked list next index (4B)
    int32_t     prevRegionIdx;      // +20  linked list prev index (4B)

    // ========== Bitmap pointers ==========
    uint64_t*   allocBitmap;        // +24  allocation bitmap: 1=free, 0=allocated (8B)
    uint64_t*   markBitmap;         // +32  mark bitmap: 1=alive, 0=dead (8B)

    // ========== Statistics ==========
    uint16_t    totalSlotCount;     // +40  total slots in Region (2B)
    uint16_t    freeSlotCount;      // +42  current free slots (2B)
    uint16_t    liveSlotCount;      // +44  live slots after last GC (2B)
    uint16_t    rawPointerObjCount; // +46  weak ref reference count (2B)

    // ========== Classification ==========
    uint8_t      sizeClass;         // +48  Size Class index (1B)
    MSSweepState sweepState;        // +49  sweep state (1B)
    MSRegionType regionType;        // +50  Region type (1B)

    // ========== Flag bits ==========
    uint8_t     isMarked     : 1;   // +51.0  GC mark flag
    uint8_t     isEnqueued   : 1;   // +51.1  weak ref queue flag
    uint8_t     isResurrected: 1;   // +51.2  finalizer resurrection flag
    uint8_t     flagsPadding : 5;   // +51.3  reserved bits

    // ========== Alignment padding ==========
    uint32_t    sweepGen;           // +52  sweep generation for lazy sweep (4B)

    // ========== Reserved for future ==========
    uint64_t    reserved;           // +56  summary word (PARTIAL: bit[i]=1 if allocBitmap[i] has free) / large obj base idx (8B)

    // === Constants ===
    static constexpr int32_t INVALID_IDX = static_cast<int32_t>(0xFFFFFFFF);

    // === Address helpers ===
    uintptr_t RegionStart() const { return regionEnd - MS_REGION_SIZE; }

    // === Lifecycle: Init / Release ===
    void InitForSizeClass(uint8_t cls);
    void InitForLargeObject(uintptr_t start, uintptr_t end);
    void ReleaseBitmaps();
    void Reset();
    // Reset a completely-free region back to BUMP for fast reuse (O3).
    // Preserves bitmap memory, avoids munmap/mmap cycle.
    void ResetToBump();

    // === Allocation ===
    // Bump-pointer allocation for fresh BUMP regions (inline for performance).
    inline uintptr_t AllocBump()
    {
        size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
        if (LIKELY(allocPtr + slotSize <= regionEnd)) {
            uintptr_t addr = allocPtr;
            allocPtr += slotSize;
            freeSlotCount--;
            return addr;
        }
        return 0;  // Region full
    }

    // Bitmap-based allocation for swept regions (inline fast path).
    // Uses summary word (reserved) for O(1) lookup: bit[i]=1 means allocBitmap[i] has free slots.
    inline uintptr_t AllocFromBitmapFast()
    {
        uint64_t summary = reserved;
        while (summary != 0) {
            size_t wordIdx = static_cast<size_t>(__builtin_ctzll(summary));
            uint64_t word = allocBitmap[wordIdx];
            if (LIKELY(word != 0)) {
                int bit = __builtin_ctzll(word);
                allocBitmap[wordIdx] = word & ~(1ULL << bit);
                freeSlotCount--;
                if (allocBitmap[wordIdx] == 0) {
                    reserved = reserved & ~(1ULL << wordIdx);
                }
                size_t slotIndex = wordIdx * 64 + static_cast<size_t>(bit);
                return RegionStart() + slotIndex * SizeClassInfo::ClassToSize(sizeClass);
            }
            // Summary was stale for this word, self-correct and try next
            summary &= ~(1ULL << wordIdx);
            reserved = reserved & ~(1ULL << wordIdx);
        }
        return 0;  // Region full
    }

    // Slow path: wrap-around search (legacy, kept for Alloc() backward compat)
    uintptr_t AllocFromBitmapSlow(size_t hint);

    // Legacy entry points (redirect to inline versions)
    uintptr_t Alloc();
    uintptr_t AllocFromBitmap();

    // === Sweep ===
    // Returns bytes freed. Sets allocBitmap = ~markBitmap.
    size_t SweepRegion();

    // === Mark ===
    void SetMarkBit(uintptr_t objAddr);
    bool GetMarkBit(uintptr_t objAddr) const;
    // Returns true if already marked (previous bit was set).
    bool TestAndSetMarkBit(uintptr_t objAddr);

    // === Sweep Generation (CAS-based lazy sweep protocol) ===
    uint32_t GetSweepGen() const { return sweepGen; }
    void SetSweepGen(uint32_t gen) { sweepGen = gen; }

    bool CasSweepGen(uint32_t expected, uint32_t desired)
    {
        return reinterpret_cast<std::atomic<uint32_t>*>(&sweepGen)
            ->compare_exchange_strong(expected, desired, std::memory_order_acq_rel);
    }

    // === Queries ===
    bool IsCompletelyFree() const { return freeSlotCount == totalSlotCount; }
    bool HasFreeSpace() const;
    size_t GetSlotSize() const { return SizeClassInfo::ClassToSize(sizeClass); }
    size_t GetAllocatedBytes() const;
    size_t GetLiveBytes() const;
};

static_assert(sizeof(MSRegionInfo) == 64, "MSRegionInfo must be exactly 64 bytes (one cache line)");
static_assert(offsetof(MSRegionInfo, allocPtr) == 0, "allocPtr must be at offset 0 for compiler fast-path");

// ============================================================================
// MSRegionInfoArray: mirror array management (heap-front layout)
// ============================================================================
class MSRegionInfoArray {
public:
    // Initialize with external array pointer (from mmap front) — no malloc.
    // If array is nullptr, falls back to malloc (for backward compatibility).
    static void Init(uintptr_t heapStart, size_t maxRegionCount, MSRegionInfo* array = nullptr);

    // O(1) address -> MSRegionInfo lookup (no branching)
    static inline MSRegionInfo* GetRegionInfo(uintptr_t addr)
    {
        size_t regionIdx = (addr - heapStartAddress_) / MS_REGION_SIZE;
        return &array_[regionIdx];
    }

    // Address -> MSRegionInfo lookup with alignment correction.
    // TakeRegion returns UNIT_SIZE-aligned addresses, which may not be
    // MS_REGION_SIZE-aligned. Objects near boundaries can map to the wrong
    // MSRegionInfo entry. This validates and tries adjacent entries.
    // Returns nullptr if no valid entry found.
    static inline MSRegionInfo* GetRegionInfoValidated(uintptr_t addr)
    {
        size_t regionIdx = (addr - heapStartAddress_) / MS_REGION_SIZE;
        MSRegionInfo* info = &array_[regionIdx];

        if (info->regionEnd != 0) {
            if (info->regionType == MSRegionType::MS_REGION_LARGE) {
                // LARGE base entry: full object range is [allocPtr, regionEnd)
                if (addr >= info->allocPtr && addr < info->regionEnd) {
                    return info;
                }
                // addr not in this large object — cross-contamination from
                // adjacent unaligned small region. Fall through to slow path.
            } else {
                // Small/medium: range is [RegionStart(), regionEnd)
                if (addr >= info->RegionStart() && addr < info->regionEnd) {
                    return info;
                }
            }
        } else if (info->regionType == MSRegionType::MS_REGION_LARGE) {
            // LARGE span entry (regionEnd==0, reserved=baseIdx).
            // RegisterLargeObjectMS initializes ALL spans; the index is
            // correct for any interior pointer into the large object.
            return info;
        }

        // Slow path: alignment mismatch for small/medium regions.
        // Only search non-LARGE adjacent entries.
        if (regionIdx > 0) {
            MSRegionInfo* prev = &array_[regionIdx - 1];
            if (prev->regionEnd != 0 &&
                prev->regionType != MSRegionType::MS_REGION_LARGE &&
                addr >= prev->RegionStart() && addr < prev->regionEnd) {
                return prev;
            }
        }
        if (regionIdx + 1 < maxRegionCount_) {
            MSRegionInfo* next = &array_[regionIdx + 1];
            if (next->regionEnd != 0 &&
                next->regionType != MSRegionType::MS_REGION_LARGE &&
                addr >= next->RegionStart() && addr < next->regionEnd) {
                return next;
            }
        }

        return nullptr;  // Cannot resolve
    }

    // O(1) index -> MSRegionInfo
    static inline MSRegionInfo* GetRegionInfoByIdx(size_t idx)
    {
        return &array_[idx];
    }

    // O(1) MSRegionInfo -> index
    static inline size_t GetRegionIdx(const MSRegionInfo* info)
    {
        return static_cast<size_t>(info - array_);
    }

    // O(1) index -> Region start address
    static inline uintptr_t GetRegionAddress(size_t idx)
    {
        return heapStartAddress_ + idx * MS_REGION_SIZE;
    }

    // Check if address is within heap range
    static inline bool IsHeapAddress(uintptr_t addr)
    {
        return addr >= heapStartAddress_ && addr < heapEndAddress_;
    }

    // Shared MarkObject implementation for heap-internal addresses.
    // Precondition: IsHeapAddress(addr) == true.
    // Returns true = already marked (skip), false = newly marked (enqueue).
    static inline bool MarkObjectInHeap(uintptr_t addr)
    {
        MSRegionInfo* msInfo = GetRegionInfoValidated(addr);
        if (msInfo == nullptr) {
            return true;  // released region — treat as already marked
        }
        if (msInfo->regionType == MSRegionType::MS_REGION_LARGE) {
            MSRegionInfo* base = GetRegionInfoByIdx(
                static_cast<size_t>(msInfo->reserved));
            // Guard: base entry already Reset (stale large region)
            if (base->allocPtr == 0 && base->regionEnd == 0) {
                return true;
            }
            if (base->isMarked) {
                return true;
            }
            base->isMarked = 1;
            return false;  // newly marked
        }
        if (msInfo->markBitmap != nullptr && msInfo->totalSlotCount > 0) {
            return msInfo->TestAndSetMarkBit(addr);
        }
        return true;  // uninitialized region — treat as already marked
    }

    static size_t GetMaxRegionCount() { return maxRegionCount_; }
    static uintptr_t GetHeapStartAddress() { return heapStartAddress_; }
    static void Cleanup();

private:
    static MSRegionInfo* array_;           // Array (mmap front or malloc fallback)
    static uintptr_t heapStartAddress_;    // First Region address
    static uintptr_t heapEndAddress_;      // Past-the-end address
    static size_t maxRegionCount_;
    static bool ownedByMalloc_;            // true if array_ was malloc'd (needs free)
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_REGION_INFO_H
