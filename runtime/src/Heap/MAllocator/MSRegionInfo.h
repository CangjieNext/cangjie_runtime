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
#include <type_traits>

#include "Base/Globals.h"
#include "Heap/MAllocator/MSSizeClass.h"

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
    MS_REGION_EMPTY_CACHE = 6, // Empty small-region shell cached in CentralRegionPool
};

// Sweep state for lazy sweep protocol
enum class MSSweepState : uint8_t {
    UNSWEPT  = 0,
    SWEPT    = 2,
};

enum class MSCentralReturnKind : uint8_t {
    EMPTY_CACHE = 0,
    PARTIAL = 1,
    FULL = 2,
};

enum class MSRegionLocation : uint8_t {
    UNKNOWN = 0,
    FREE,
    TRANSIENT,
    ACTIVE_LOCAL,
    LOCAL_EMPTY_CACHE,
    GLOBAL_EMPTY_CACHE,
    CENTRAL_PARTIAL,
    CENTRAL_FULL,
    SWEEP_CLAIMED,
    LARGE_ACTIVE,
};

struct MSCentralReturnDecision {
    MSCentralReturnKind kind;
    uint8_t bucketSizeClass;
    bool callerClassMismatch;
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
    static constexpr uint8_t INVALID_SIZE_CLASS = 0xFF;

    // ========== Allocation core (offset 0, compiler fast-path) ==========
    uintptr_t   allocPtr;           // +0   bump-pointer current position (8B)
    uintptr_t   regionEnd;          // +8   Region end address (8B)

    // ========== Region list management ==========
    int32_t     nextRegionIdx;      // +16  linked list next index (4B)
    int32_t     prevRegionIdx;      // +20  linked list prev index (4B)

    // ========== Bitmap pointers ==========
    // Allocation is performed only by the owning AllocBufferMS/P. The Runtime
    // and compiler fast paths intentionally update this bitmap with ordinary
    // loads/stores; Region ownership prevents concurrent allocators.
    uint64_t*   allocBitmap;        // +24  allocation bitmap: 1=free, 0=allocated (8B)
    uint64_t*   markBitmap;         // +32  mark bitmap: 1=alive, 0=dead (8B)

    // ========== Statistics ==========
    // LARGE has no slots and packs its exact logical object size across these
    // otherwise-unused 64 bits (low 16 / middle 16 / high 32).
    uint16_t    totalSlotCount;     // +40  total slots / large object bytes[15:0] (2B)
    uint16_t    freeSlotCount;      // +42  free slots / large object bytes[31:16] (2B)
    uint32_t    divMagic;           // +44  slot div magic / large object bytes[63:32] (4B)

    // ========== Classification ==========
    uint8_t      sizeClass;         // +48  Size Class index (1B)
    MSSweepState sweepState;        // +49  sweep state (1B)
    MSRegionType regionType;        // +50  Region type (1B)

    // ========== Flag bits (atomic for large-object concurrent safety) ==========
    // bit 0: isMarked
    static constexpr uint8_t FLAG_MARKED = 0x01;
    uint8_t     flags_;              // +51  all GC flag bits (use atomic accessors for large objects)

    // Atomic accessors — safe for concurrent mark on large objects
    bool IsMarked() const { return (__atomic_load_n(&flags_, __ATOMIC_RELAXED) & FLAG_MARKED) != 0; }

    // Returns true if was already marked (skip), false if newly marked (enqueue)
    bool AtomicTestAndSetMarked() {
        uint8_t old = __atomic_fetch_or(&flags_, FLAG_MARKED, __ATOMIC_RELAXED);
        return (old & FLAG_MARKED) != 0;
    }
    void AtomicSetMarked() { __atomic_fetch_or(&flags_, FLAG_MARKED, __ATOMIC_RELAXED); }
    void ClearMarkedFlag() {
        uint8_t old = __atomic_load_n(&flags_, __ATOMIC_RELAXED);
        uint8_t nv = old & ~FLAG_MARKED;
        __atomic_store_n(&flags_, nv, __ATOMIC_RELAXED);
    }

    // ========== Alignment padding ==========
    uint32_t    sweepGen;           // +52  sweep generation for lazy sweep (4B)

    // ========== Reserved for future ==========
    uint64_t    reserved;           // +56  summary word (PARTIAL: bit[i]=1 if allocBitmap[i] has free) / large obj base idx (8B)

    // === Constants ===
    static constexpr int32_t INVALID_IDX = static_cast<int32_t>(0xFFFFFFFF);

    // === Address helpers ===
    // RegionStart() describes one small Region. Large objects may span several
    // Regions and must use the explicit Large* helpers below.
    uintptr_t RegionStart() const { return regionEnd - MS_REGION_SIZE; }
    uintptr_t LargeObjectStart() const { return allocPtr; }
    size_t LargeObjectSize() const
    {
        uint64_t objectBytes = (static_cast<uint64_t>(divMagic) << 32) |
            (static_cast<uint64_t>(freeSlotCount) << 16) |
            static_cast<uint64_t>(totalSlotCount);
        return static_cast<size_t>(objectBytes);
    }
    size_t LargeSpanBytes() const { return regionEnd - allocPtr; }
    size_t LargeSpanCount() const { return LargeSpanBytes() / MS_REGION_SIZE; }

    // === Lifecycle: Init / Release ===
    void InitForSizeClass(uint8_t cls);
    void InitForLargeObject(uintptr_t start, uintptr_t end, size_t objectBytes);
    void ReleaseBitmaps();
    void Reset();
    // Reset a completely-free region back to BUMP for fast reuse (O3).
    // Preserves bitmap memory, avoids munmap/mmap cycle.
    void ResetToBump();
    // Convert a completely-free small region into a classless empty-cache shell.
    // Releases per-class bitmaps and clears object-layout metadata, but does not
    // return the physical region to MSRegionManager.
    void ResetToEmptyCache();

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

    // Bitmap-based allocation for swept regions owned exclusively by this P.
    // Keep the ordinary load/store updates in sync with the compiler emitter;
    // no atomic RMW or lock is required while the Region remains P-local.
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

    // === Sweep ===
    // Returns bytes freed. Sets allocBitmap = ~markBitmap.
    size_t SweepRegion();

    // === Mark ===
    bool GetMarkBit(uintptr_t objAddr) const;
    // Tests a previously resolved bitmap location without recomputing the
    // object-to-slot mapping. Invalid bitmap coordinates are treated as white.
    bool IsMarkBitSet(size_t wordIdx, uint64_t bitMask) const;
    // Returns true if already marked (previous bit was set).
    bool TestAndSetMarkBit(uintptr_t objAddr);
    // Atomically marks a bitmap word and returns only bits newly set by this call.
    uint64_t MarkBitmapWord(size_t wordIdx, uint64_t mask);
    // Clear markBitmap for next GC cycle. Called by FlushAll during MarkSetup STW
    // to clear mark-at-alloc dirty bits on TL regions before returning to pool.
    void ClearMarkBitmap()
    {
        if (markBitmap != nullptr && totalSlotCount > 0) {
            size_t bitmapWords = (totalSlotCount + 63) / 64;
            memset(markBitmap, 0, bitmapWords * sizeof(uint64_t));
        }
    }

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
    MSCentralReturnDecision PrepareForCentralReturn(uint8_t callerCls)
    {
        MSCentralReturnDecision decision {
            MSCentralReturnKind::EMPTY_CACHE,
            sizeClass,
            callerCls != sizeClass,
        };
        if (IsCompletelyFree()) {
            regionType = MSRegionType::MS_REGION_EMPTY_CACHE;
            return decision;
        }
        if (HasFreeSpace()) {
            if (regionType == MSRegionType::MS_REGION_BUMP) {
                MaterializeBumpAllocationBitmap();
            }
            regionType = MSRegionType::MS_REGION_PARTIAL;
            decision.kind = MSCentralReturnKind::PARTIAL;
            return decision;
        }
        regionType = MSRegionType::MS_REGION_FULL;
        decision.kind = MSCentralReturnKind::FULL;
        return decision;
    }
    size_t GetSlotSize() const { return SizeClassInfo::ClassToSize(sizeClass); }

private:
    // BUMP allocation does not update allocBitmap on the hot path. Before a
    // partially-used BUMP region is published as PARTIAL, materialize the
    // allocated prefix and rebuild the free-word summary used by bitmap alloc.
    void MaterializeBumpAllocationBitmap();
};

static_assert(std::is_standard_layout<MSRegionInfo>::value,
              "MSRegionInfo must remain standard-layout for compiler ABI checks");
static_assert(sizeof(MSRegionInfo) == 64, "MSRegionInfo must be exactly 64 bytes (one cache line)");
static_assert(offsetof(MSRegionInfo, allocPtr) == 0, "compiler ABI: MSRegionInfo::allocPtr");
#if UINTPTR_MAX == UINT64_MAX
static_assert(offsetof(MSRegionInfo, regionEnd) == 8, "compiler ABI: MSRegionInfo::regionEnd");
static_assert(offsetof(MSRegionInfo, allocBitmap) == 24, "compiler ABI: MSRegionInfo::allocBitmap");
static_assert(offsetof(MSRegionInfo, markBitmap) == 32, "compiler ABI: MSRegionInfo::markBitmap");
static_assert(offsetof(MSRegionInfo, totalSlotCount) == 40,
              "compiler ABI: MSRegionInfo::totalSlotCount");
static_assert(offsetof(MSRegionInfo, freeSlotCount) == 42,
              "compiler ABI: MSRegionInfo::freeSlotCount");
static_assert(offsetof(MSRegionInfo, divMagic) == 44, "compiler ABI: MSRegionInfo::divMagic");
static_assert(offsetof(MSRegionInfo, regionType) == 50, "compiler ABI: MSRegionInfo::regionType");
static_assert(offsetof(MSRegionInfo, flags_) == 51, "compiler ABI: MSRegionInfo::flags_");
static_assert(offsetof(MSRegionInfo, reserved) == 56, "compiler ABI: MSRegionInfo::reserved");
#endif
static_assert(MS_REGION_SIZE == 32768, "compiler ABI: MSGC Region size");
static_assert(static_cast<uint8_t>(MSRegionType::MS_REGION_BUMP) == 1,
              "compiler ABI: BUMP Region type");
static_assert(static_cast<uint8_t>(MSRegionType::MS_REGION_PARTIAL) == 2,
              "compiler ABI: PARTIAL Region type");
static_assert(static_cast<uint8_t>(MSRegionType::MS_REGION_FULL) == 3,
              "compiler ABI: FULL Region type");
static_assert(static_cast<uint8_t>(MSRegionType::MS_REGION_LARGE) == 4,
              "compiler ABI: LARGE Region type");

// ============================================================================
// MSRegionInfoArray: mirror array management (heap-front layout)
// ============================================================================
class MSRegionInfoArray {
public:
    static constexpr uintptr_t ANY_OWNER = static_cast<uintptr_t>(-1);

    // Initialize with external array pointer (from mmap front) — no malloc.
    // If array is nullptr, falls back to malloc (for backward compatibility).
    static void Init(uintptr_t heapStart, size_t maxRegionCount, MSRegionInfo* array = nullptr);

    static bool TryGetTrackedIdx(const MSRegionInfo* info, size_t& idx);
    static MSRegionLocation GetLocation(const MSRegionInfo* info);
    static uintptr_t GetOwnerToken(const MSRegionInfo* info);
    static void SetState(const MSRegionInfo* info, MSRegionLocation location, uintptr_t ownerToken = 0);
    static void TransitionState(const MSRegionInfo* info, MSRegionLocation expectedLocation,
                                MSRegionLocation newLocation, uintptr_t expectedOwnerToken = ANY_OWNER,
                                uintptr_t newOwnerToken = 0, const char* context = nullptr);
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
                // LARGE base entry: only object base pointer is valid.
                if (addr == info->allocPtr) {
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
            // Validate base entry liveness/range to avoid treating stale
            // span metadata (after large object reclaimed/reset) as valid.
            MSRegionInfo* base = GetRegionInfoByIdx(static_cast<size_t>(info->reserved));
            if (base->regionType != MSRegionType::MS_REGION_LARGE ||
                base->allocPtr == 0 || base->regionEnd == 0) {
                return nullptr;
            }
            if (addr == base->allocPtr) {
                return info;
            }
            return nullptr;
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
        return MarkObjectInHeapKnown(msInfo, addr);
    }

    // Same as MarkObjectInHeap but for a region the caller has already resolved
    // via GetRegionInfoValidated(addr) (e.g. IsTraceableObjectAddress validated
    // the very same addr just before). Avoids the duplicate region lookup +
    // LARGE base resolution on the flush/discover hot path.
    // Precondition: msInfo != nullptr && msInfo == GetRegionInfoValidated(addr).
    static inline bool MarkObjectInHeapKnown(MSRegionInfo* msInfo, uintptr_t addr)
    {
        if (msInfo->regionType == MSRegionType::MS_REGION_LARGE) {
            MSRegionInfo* base = GetRegionInfoByIdx(
                static_cast<size_t>(msInfo->reserved));
            // Guard: base entry already Reset (stale large region)
            if (base->allocPtr == 0 && base->regionEnd == 0) {
                return true;
            }
            return base->AtomicTestAndSetMarked();
        }
        if (msInfo->markBitmap != nullptr && msInfo->totalSlotCount > 0) {
            return msInfo->TestAndSetMarkBit(addr);
        }
        return true;  // uninitialized region — treat as already marked
    }

    static size_t GetMaxRegionCount() { return maxRegionCount_; }
    static void Cleanup();

private:
    static MSRegionInfo* array_;           // Array (mmap front or malloc fallback)
    static uintptr_t heapStartAddress_;    // First Region address
    static uintptr_t heapEndAddress_;      // Past-the-end address
    static size_t maxRegionCount_;
    static bool ownedByMalloc_;            // true if array_ was malloc'd (needs free)
    static std::atomic<uint8_t>* locations_;
    static std::atomic<uintptr_t>* ownerTokens_;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_REGION_INFO_H
