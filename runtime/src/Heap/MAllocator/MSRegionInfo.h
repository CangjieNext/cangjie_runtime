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
#include "Heap/MAllocator/MSBitmapArena.h"
#include "Heap/MAllocator/MSScanClass.h"
#include "Heap/MAllocator/MSSizeClass.h"
#include "Heap/MSGCFastPath.h"

namespace MapleRuntime {

// ============================================================================
// MSRegionType: Region lifecycle states for Mark-Sweep GC
// ============================================================================
// Simplified from CopyGC's 15+ RegionType to a compact allocation/lifecycle ABI.
//
// State transitions:
//   FREE --alloc--> BUMP --full--> FULL --sweep--> PARTIAL
//   PARTIAL --TLS acquire--> BITMAP --full/return--> FULL/PARTIAL
//   FREE --allocLarge--> LARGE --gcSweepDead--> FREE
//
enum class MSRegionType : uint8_t {
    MS_REGION_FREE    = 0,  // Unallocated, in free pool
    MS_REGION_BUMP    = 1,  // First-fill using bump-pointer (allocPtr < regionEnd)
    MS_REGION_BITMAP  = 2,  // TLS-owned small Region allocating from allocBitmap
    MS_REGION_PARTIAL = 3,  // Central swept Region with free slots
    MS_REGION_FULL    = 4,  // Central Region with no free slots
    MS_REGION_LARGE   = 5,  // Base Region of one large object
    MS_REGION_SPAN    = 6,  // Secondary Region of a large object; spanBaseRegionIdx=baseIdx
    MS_REGION_EMPTY_CACHE = 7, // Empty small-region shell cached in CentralRegionPool
};

enum class MSRegionKind : uint8_t {
    FREE = 0,
    SMALL = 1,
    LARGE = 2,
    SPAN = 3,
};

enum class MSAllocMode : uint8_t {
    NONE = 0,
    BUMP = 1,
    BITMAP = 2,
};

enum class MSScanState : uint8_t {
    IDLE = 0,
    QUEUED = 1,
    OWNED = 2,
};

enum class MSCentralReturnKind : uint8_t {
    EMPTY_CACHE = 0,
    PARTIAL = 1,
    FULL = 2,
};

struct MSCentralReturnDecision {
    MSCentralReturnKind kind;
    uint8_t bucketSizeClass;
    bool callerClassMismatch;
    uint16_t freeSlots;
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
struct alignas(64) MSRegionInfo {
    static constexpr uint8_t INVALID_SIZE_CLASS = 0xFF;
    static constexpr uint32_t INVALID_REGION_IDX = UINT32_MAX;
    static constexpr int32_t INVALID_IDX = static_cast<int32_t>(INVALID_REGION_IDX);
    static constexpr uint32_t NO_COLLECT_EPOCH = 0;

    uintptr_t allocPtr;                   // +0
    uintptr_t regionEnd;                  // +8
    uint32_t centralNextRegionIdx;        // +16
    uint32_t markTaskNextRegionIdx;       // +20
    union {
        uint64_t* allocBitmap;            // +24, SMALL: 1=free, 0=allocated
        uint64_t largeObjectBytes;        // +24, LARGE: exact logical object size
    };
    uint32_t collectEpoch;                // +32
    uint32_t control;                     // +36
    uint64_t allocSummary;                // +40
    uint64_t pendingWordSummary;          // +48
    // SPAN secondary: primary Region index. INVALID_REGION_IDX otherwise.
    uint32_t spanBaseRegionIdx;           // +56
    uint8_t allocMode;                    // +60
    uint8_t reserved8;                    // +61
    uint8_t sizeClass;                    // +62
    uint8_t layout;                       // +63

    static constexpr uint8_t LAYOUT_REGION_KIND_MASK = 0x03;
    static constexpr uint8_t LAYOUT_SCAN_CLASS_MASK = 0x08;
    static constexpr uint32_t CONTROL_SCAN_STATE_MASK = 0x03;
    static constexpr uint32_t CONTROL_LARGE_MARKED = 0x04;
    // Small Region currently parked in central swept supply after a real
    // mutator return. This provenance shares the existing control word.
    static constexpr uint32_t CONTROL_MUTATOR_RETURNED_SWEPT = 0x08;

    uint32_t GetCentralNextRegionIdx() const
    {
        return __atomic_load_n(&centralNextRegionIdx, __ATOMIC_RELAXED);
    }
    void SetCentralNextRegionIdx(uint32_t idx)
    {
        __atomic_store_n(&centralNextRegionIdx, idx, __ATOMIC_RELAXED);
    }
    uint32_t GetMarkTaskNextRegionIdx() const
    {
        return __atomic_load_n(&markTaskNextRegionIdx, __ATOMIC_RELAXED);
    }
    void SetMarkTaskNextRegionIdx(uint32_t idx)
    {
        __atomic_store_n(&markTaskNextRegionIdx, idx, __ATOMIC_RELAXED);
    }

    uint16_t GetTotalSlotCount() const
    {
        return sizeClass < kNumSizeClasses ? SizeClassInfo::ClassToTotalSlots(sizeClass) : 0;
    }
    uint8_t GetBitmapWordCount() const
    {
        return sizeClass < kNumSizeClasses ? SizeClassInfo::ClassToBitmapWords(sizeClass) : 0;
    }
    uint64_t GetValidTailMask() const
    {
        return sizeClass < kNumSizeClasses ? SizeClassInfo::ClassToValidTailMask(sizeClass) : 0;
    }
    size_t GetRegionIndex() const
    {
        uintptr_t arrayBase = static_cast<uintptr_t>(
            __atomic_load_n(&CJ_MRT_MSGCFastPathState.regionInfoArray, __ATOMIC_ACQUIRE));
        return (reinterpret_cast<uintptr_t>(this) - arrayBase) / sizeof(MSRegionInfo);
    }
    uint64_t* GetAllocBitmap() const
    {
        return allocBitmap;
    }
    uint64_t* GetMarkBitmap() const
    {
        uintptr_t base = static_cast<uintptr_t>(
            __atomic_load_n(&CJ_MRT_MSGCFastPathState.markPlaneBase, __ATOMIC_ACQUIRE));
        return reinterpret_cast<uint64_t*>(
            base + GetRegionIndex() * MSBitmapArena::kPlaneStrideBytes);
    }
    uint64_t* GetScanBitmap() const
    {
        uintptr_t base = static_cast<uintptr_t>(
            __atomic_load_n(&CJ_MRT_MSGCFastPathState.scanPlaneBase, __ATOMIC_ACQUIRE));
        return reinterpret_cast<uint64_t*>(
            base + GetRegionIndex() * MSBitmapArena::kPlaneStrideBytes);
    }

    bool IsMarked() const
    {
        return (__atomic_load_n(&control, __ATOMIC_RELAXED) & CONTROL_LARGE_MARKED) != 0;
    }
    bool AtomicTestAndSetMarked()
    {
        uint32_t old = __atomic_fetch_or(&control, CONTROL_LARGE_MARKED, __ATOMIC_RELAXED);
        return (old & CONTROL_LARGE_MARKED) != 0;
    }
    void AtomicSetMarked()
    {
        __atomic_fetch_or(&control, CONTROL_LARGE_MARKED, __ATOMIC_RELAXED);
    }
    void ClearMarkedFlag()
    {
        __atomic_fetch_and(&control, ~CONTROL_LARGE_MARKED, __ATOMIC_RELAXED);
    }
    bool IsMutatorReturnedSwept() const
    {
        return (__atomic_load_n(&control, __ATOMIC_ACQUIRE) &
            CONTROL_MUTATOR_RETURNED_SWEPT) != 0;
    }
    void MarkMutatorReturnedSwept()
    {
        __atomic_fetch_or(&control, CONTROL_MUTATOR_RETURNED_SWEPT,
                          __ATOMIC_RELEASE);
    }
    void ClearMutatorReturnedSwept()
    {
        __atomic_fetch_and(&control, ~CONTROL_MUTATOR_RETURNED_SWEPT,
                           __ATOMIC_RELEASE);
    }

    MSRegionKind GetRegionKind() const
    {
        return static_cast<MSRegionKind>(layout & LAYOUT_REGION_KIND_MASK);
    }
    void SetRegionKind(MSRegionKind kind)
    {
        layout = static_cast<uint8_t>(
            (layout & ~LAYOUT_REGION_KIND_MASK) | static_cast<uint8_t>(kind));
    }
    MSScanClass GetScanClass() const
    {
        return (layout & LAYOUT_SCAN_CLASS_MASK) != 0 ? MSScanClass::HAS_REF : MSScanClass::NO_REF;
    }
    void SetScanClass(MSScanClass scanClass)
    {
        layout = static_cast<uint8_t>(
            (layout & ~LAYOUT_SCAN_CLASS_MASK) |
            (scanClass == MSScanClass::HAS_REF ? LAYOUT_SCAN_CLASS_MASK : 0));
    }
    MSAllocMode GetAllocMode() const { return static_cast<MSAllocMode>(allocMode); }
    void SetAllocMode(MSAllocMode mode) { allocMode = static_cast<uint8_t>(mode); }
    MSScanState GetScanState() const
    {
        return static_cast<MSScanState>(
            __atomic_load_n(&control, __ATOMIC_ACQUIRE) & CONTROL_SCAN_STATE_MASK);
    }
    bool CasScanState(MSScanState expectedState, MSScanState desiredState)
    {
        uint32_t old = __atomic_load_n(&control, __ATOMIC_ACQUIRE);
        for (;;) {
            if ((old & CONTROL_SCAN_STATE_MASK) !=
                static_cast<uint32_t>(expectedState)) {
                return false;
            }
            uint32_t desired =
                (old & ~CONTROL_SCAN_STATE_MASK) |
                static_cast<uint32_t>(desiredState);
            if (__atomic_compare_exchange_n(
                &control, &old, desired, true,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return true;
            }
        }
    }
    uint64_t PublishPendingMarkWords(uint64_t wordMask)
    {
        if (wordMask == 0) {
            return 0;
        }
        // A barrier-buffer flush may win several mark words in one Region.
        // Publish them with one release RMW before the single Region enqueue
        // attempt, preserving the existing summary-first protocol.
        uint64_t old = __atomic_fetch_or(
            &pendingWordSummary, wordMask, __ATOMIC_RELEASE);
        return wordMask & ~old;
    }
    uint64_t TakePendingMarkWords()
    {
        return __atomic_exchange_n(
            &pendingWordSummary, UINT64_C(0), __ATOMIC_ACQUIRE);
    }
    uint64_t LoadPendingMarkWordsAcquire() const
    {
        return __atomic_load_n(&pendingWordSummary, __ATOMIC_ACQUIRE);
    }

    MSRegionType GetRegionType() const
    {
        switch (GetRegionKind()) {
            case MSRegionKind::LARGE:
                return MSRegionType::MS_REGION_LARGE;
            case MSRegionKind::SPAN:
                return MSRegionType::MS_REGION_SPAN;
            case MSRegionKind::SMALL:
                return GetAllocMode() == MSAllocMode::BUMP
                    ? MSRegionType::MS_REGION_BUMP : MSRegionType::MS_REGION_BITMAP;
            case MSRegionKind::FREE:
            default:
                return MSRegionType::MS_REGION_FREE;
        }
    }
    void SetRegionType(MSRegionType type)
    {
        switch (type) {
            case MSRegionType::MS_REGION_BUMP:
                SetRegionKind(MSRegionKind::SMALL);
                SetAllocMode(MSAllocMode::BUMP);
                break;
            case MSRegionType::MS_REGION_BITMAP:
            case MSRegionType::MS_REGION_PARTIAL:
            case MSRegionType::MS_REGION_FULL:
                SetRegionKind(MSRegionKind::SMALL);
                SetAllocMode(MSAllocMode::BITMAP);
                break;
            case MSRegionType::MS_REGION_LARGE:
                SetRegionKind(MSRegionKind::LARGE);
                SetAllocMode(MSAllocMode::NONE);
                break;
            case MSRegionType::MS_REGION_SPAN:
                SetRegionKind(MSRegionKind::SPAN);
                SetAllocMode(MSAllocMode::NONE);
                break;
            case MSRegionType::MS_REGION_FREE:
            case MSRegionType::MS_REGION_EMPTY_CACHE:
            default:
                SetRegionKind(MSRegionKind::FREE);
                SetAllocMode(MSAllocMode::NONE);
                break;
        }
    }
    bool IsLargeRegionEntry() const
    {
        return GetRegionKind() == MSRegionKind::LARGE || GetRegionKind() == MSRegionKind::SPAN;
    }
    bool IsFreeRegion() const { return GetRegionKind() == MSRegionKind::FREE; }
    uint32_t GetSpanBaseRegionIdx() const
    {
        return spanBaseRegionIdx;
    }
    void SetSpanBaseRegionIdx(uint32_t idx)
    {
        spanBaseRegionIdx = idx;
    }

    // === Address helpers ===
    // RegionStart() describes one small Region. Large objects may span several
    // Regions and must use the explicit Large* helpers below.
    uintptr_t RegionStart() const { return regionEnd - MS_REGION_SIZE; }
    uintptr_t LargeObjectStart() const { return allocPtr; }
    size_t LargeObjectSize() const
    {
        return static_cast<size_t>(largeObjectBytes);
    }
    size_t LargeSpanBytes() const { return regionEnd - allocPtr; }
    size_t LargeSpanCount() const { return LargeSpanBytes() / MS_REGION_SIZE; }

    // === Lifecycle: Init / Release ===
    void InitForSizeClass(uint8_t cls, MSScanClass scanClass);
    void InitForLargeObject(uintptr_t start, uintptr_t end, size_t objectBytes, MSScanClass scanClass);
    void ReleaseBitmaps();
    void Reset();
    // Reset a completely-free region back to BUMP for fast reuse (O3).
    // Preserves the Arena-owned bitmap storage and reuses this Region as BUMP.
    void ResetToBump();
    // Convert a completely-free small region into a classless empty-cache shell.
    // Clears per-class bitmap bindings and object-layout metadata, but does not
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
            return addr;
        }
        return 0;  // Region full
    }

    // Bitmap-based allocation for swept regions owned exclusively by this P.
    // Keep the ordinary load/store updates in sync with the compiler emitter;
    // no atomic RMW or lock is required while the Region remains P-local.
    // Uses allocSummary for O(1) lookup: bit[i]=1 means the alloc word has free slots.
    inline uintptr_t AllocFromBitmapFast()
    {
        uint64_t* allocBitmap = GetAllocBitmap();
        uint64_t summary = allocSummary;
        while (summary != 0) {
            size_t wordIdx = static_cast<size_t>(__builtin_ctzll(summary));
            uint64_t word = allocBitmap[wordIdx];
            if (LIKELY(word != 0)) {
                int bit = __builtin_ctzll(word);
                allocBitmap[wordIdx] = word & ~(1ULL << bit);
                if (allocBitmap[wordIdx] == 0) {
                    allocSummary &= ~(1ULL << wordIdx);
                }
                size_t slotIndex = wordIdx * 64 + static_cast<size_t>(bit);
                return RegionStart() + slotIndex * SizeClassInfo::ClassToSize(sizeClass);
            }
            // Summary was stale for this word, self-correct and try next
            summary &= ~(1ULL << wordIdx);
            allocSummary &= ~(1ULL << wordIdx);
        }
        return 0;  // Region full
    }

    // === Sweep ===
    // Rebuilds the allocation state and returns the exact Central route.
    // Empty Sweep deliberately leaves allocBitmap unmaterialized; its result
    // therefore comes from liveCount rather than a post-Sweep bitmap scan.
    MSCentralReturnDecision SweepRegion();

    // === Mark ===
    bool GetMarkBit(uintptr_t objAddr) const;
    // Tests a previously resolved bitmap location without recomputing the
    // object-to-slot mapping. Invalid bitmap coordinates are treated as white.
    bool IsMarkBitSet(size_t wordIdx, uint64_t bitMask) const;
    // Trusted-coordinate variant for a ResolvedMarkTarget that has already
    // validated markBitmap, wordIdx and bitMask.
    bool IsMarkBitSetResolved(size_t wordIdx, uint64_t bitMask) const;
    // Returns true if already marked (previous bit was set).
    bool TestAndSetMarkBit(uintptr_t objAddr);
    // Atomically marks a bitmap word and returns only bits newly set by this call.
    uint64_t MarkBitmapWord(size_t wordIdx, uint64_t mask);
    // Trusted-coordinate variant for a ResolvedMarkTarget or validated bulk
    // marker. Returns only bits newly set by this call.
    uint64_t MarkBitmapWordResolved(size_t wordIdx, uint64_t mask);
    // STW/quiescent cycle initialization. ALLOC is intentionally untouched:
    // collect-set ALLOC is the sealed allocation snapshot, while ACTIVE ALLOC
    // may still be a non-materialized BUMP view owned by its mutator.
    void ResetSmallMarkWorkState()
    {
        if (GetRegionKind() != MSRegionKind::SMALL || GetTotalSlotCount() == 0) {
            return;
        }
        size_t bitmapBytes = GetBitmapWordCount() * sizeof(uint64_t);
        memset(GetMarkBitmap(), 0, bitmapBytes);
        if (GetScanClass() == MSScanClass::HAS_REF) {
            memset(GetScanBitmap(), 0, bitmapBytes);
        }
        __atomic_store_n(&pendingWordSummary, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&control, 0, __ATOMIC_RELAXED);
        SetMarkTaskNextRegionIdx(INVALID_REGION_IDX);
    }

    // === Queries ===
    // Exact but cold: intended for return-boundary diagnostics, not allocation.
    uint16_t CountFreeSlots() const;
    MSCentralReturnDecision PrepareForCentralReturn(uint8_t callerCls);
    size_t GetSlotSize() const { return SizeClassInfo::ClassToSize(sizeClass); }

private:
    uint16_t DeriveBumpFreeSlots() const;
    uint16_t CanonicalizeBitmapFreeSlots();
    MSCentralReturnDecision BuildCentralReturnDecision(
        uint8_t callerCls, uint16_t freeSlots) const;
    // BUMP allocation does not update allocBitmap on the hot path. Before a
    // non-empty BUMP Region is published as PARTIAL or FULL, materialize the
    // allocated prefix and rebuild the free-word summary used by bitmap alloc.
    void MaterializeBumpAllocationBitmap();
};

static_assert(std::is_standard_layout<MSRegionInfo>::value,
              "MSRegionInfo must remain standard-layout for compiler ABI checks");
static_assert(sizeof(MSRegionInfo) == 64, "MSRegionInfo must be exactly 64 bytes (one cache line)");
static_assert(alignof(MSRegionInfo) == 64, "MSRegionInfo must be cache-line aligned");
static_assert(offsetof(MSRegionInfo, allocPtr) == 0, "compiler ABI: MSRegionInfo::allocPtr");
#if UINTPTR_MAX == UINT64_MAX
static_assert(offsetof(MSRegionInfo, regionEnd) == 8, "compiler ABI: MSRegionInfo::regionEnd");
static_assert(offsetof(MSRegionInfo, centralNextRegionIdx) == 16,
              "compiler ABI: MSRegionInfo::centralNextRegionIdx");
static_assert(offsetof(MSRegionInfo, markTaskNextRegionIdx) == 20,
              "compiler ABI: MSRegionInfo::markTaskNextRegionIdx");
static_assert(offsetof(MSRegionInfo, allocBitmap) == 24,
              "compiler ABI: MSRegionInfo::allocBitmap");
static_assert(offsetof(MSRegionInfo, largeObjectBytes) == 24,
              "compiler ABI: MSRegionInfo::largeObjectBytes");
static_assert(offsetof(MSRegionInfo, collectEpoch) == 32,
              "compiler ABI: MSRegionInfo::collectEpoch");
static_assert(offsetof(MSRegionInfo, control) == 36,
              "compiler ABI: MSRegionInfo::control");
static_assert(offsetof(MSRegionInfo, allocSummary) == 40,
              "compiler ABI: MSRegionInfo::allocSummary");
static_assert(offsetof(MSRegionInfo, pendingWordSummary) == 48,
              "compiler ABI: MSRegionInfo::pendingWordSummary");
static_assert(offsetof(MSRegionInfo, spanBaseRegionIdx) == 56,
              "MSRegionInfo layout: spanBaseRegionIdx");
static_assert(offsetof(MSRegionInfo, allocMode) == 60,
              "compiler ABI: MSRegionInfo::allocMode");
static_assert(offsetof(MSRegionInfo, reserved8) == 61,
              "compiler ABI: MSRegionInfo::reserved8");
static_assert(offsetof(MSRegionInfo, sizeClass) == 62,
              "compiler ABI: MSRegionInfo::sizeClass");
static_assert(offsetof(MSRegionInfo, layout) == 63,
              "compiler ABI: MSRegionInfo::layout");
#endif
static_assert(MS_REGION_SIZE == 32768, "compiler ABI: MSGC Region size");
static_assert(static_cast<uint8_t>(MSRegionType::MS_REGION_BUMP) == 1,
              "compiler ABI: BUMP Region type");
static_assert(static_cast<uint8_t>(MSRegionType::MS_REGION_BITMAP) == 2,
              "compiler ABI: BITMAP Region type");
static_assert(static_cast<uint8_t>(MSRegionType::MS_REGION_PARTIAL) == 3,
              "compiler ABI: PARTIAL Region type");
static_assert(static_cast<uint8_t>(MSRegionType::MS_REGION_FULL) == 4,
              "compiler ABI: FULL Region type");
static_assert(static_cast<uint8_t>(MSRegionType::MS_REGION_LARGE) == 5,
              "compiler ABI: LARGE Region type");
static_assert(static_cast<uint8_t>(MSRegionType::MS_REGION_SPAN) == 6,
              "compiler ABI: SPAN Region type");

// ============================================================================
// MSRegionInfoArray: mirror array management (heap-front layout)
// ============================================================================
class MSRegionInfoArray {
public:
    // Initialize with external array pointer (from mmap front). If array is
    // nullptr, use a cache-line-aligned standalone/test allocation.
    static void Init(uintptr_t heapStart, size_t maxRegionCount, MSRegionInfo* array = nullptr);

    // O(1) address -> MSRegionInfo lookup (no branching)
    static inline MSRegionInfo* GetRegionInfo(uintptr_t addr)
    {
        size_t regionIdx = (addr - heapStartAddress_) / MS_REGION_SIZE;
        return &array_[regionIdx];
    }

    // Mark hot-path coordinate lookup. MSRegionManager and FreeManager allocate
    // in MS_REGION_SIZE indexes relative to heapStartAddress_, so one subtraction
    // yields both the exact metadata entry and the in-Region object offset.
    // Precondition: addr belongs to the MSGC heap reservation.
    static inline MSRegionInfo* GetRegionInfoDirect(
        uintptr_t addr, size_t& regionOffset, size_t& regionIdx)
    {
        size_t heapOffset = static_cast<size_t>(addr - heapStartAddress_);
        regionOffset = heapOffset & (MS_REGION_SIZE - 1);
        regionIdx = heapOffset >> 15;
        return &array_[regionIdx];
    }

    // Resolve a LARGE base entry from either the base itself or one of its
    // secondary SPAN entries. Returns nullptr for stale or malformed metadata.
    static inline MSRegionInfo* ResolveLargeBaseInfo(MSRegionInfo* info)
    {
        if (info == nullptr) {
            return nullptr;
        }
        if (info->GetRegionKind() == MSRegionKind::LARGE) {
            return (info->allocPtr != 0 && info->regionEnd != 0) ? info : nullptr;
        }
        if (info->GetRegionKind() != MSRegionKind::SPAN) {
            return nullptr;
        }

        size_t baseIdx = static_cast<size_t>(info->GetSpanBaseRegionIdx());
        if (baseIdx >= maxRegionCount_) {
            return nullptr;
        }
        MSRegionInfo* base = &array_[baseIdx];
        if (base->GetRegionKind() != MSRegionKind::LARGE ||
            base->allocPtr == 0 || base->regionEnd == 0) {
            return nullptr;
        }
        return base;
    }

    // Defensive address -> MSRegionInfo lookup retained for non-Mark callers.
    // The adjacent-entry fallback is historical; current Region allocation is
    // strictly indexed by MS_REGION_SIZE relative to heapStartAddress_.
    // Returns nullptr if no live entry accepts the address.
    static inline MSRegionInfo* GetRegionInfoValidated(uintptr_t addr)
    {
        size_t regionIdx = (addr - heapStartAddress_) / MS_REGION_SIZE;
        MSRegionInfo* info = &array_[regionIdx];

        if (info->GetRegionKind() == MSRegionKind::LARGE) {
            // LARGE base entry: only object base pointer is valid.
            if (info->regionEnd != 0 && addr == info->allocPtr) {
                return info;
            }
            // addr not in this large object — cross-contamination from
            // adjacent unaligned small region. Fall through to slow path.
        } else if (info->GetRegionKind() == MSRegionKind::SPAN) {
            // SPAN entry (spanBaseRegionIdx=baseIdx).
            // Validate base entry liveness/range to avoid treating stale
            // span metadata (after large object reclaimed/reset) as valid.
            MSRegionInfo* base = ResolveLargeBaseInfo(info);
            if (base == nullptr) {
                return nullptr;
            }
            if (addr == base->allocPtr) {
                return info;
            }
            return nullptr;
        } else if (info->regionEnd != 0) {
            // Small/medium: range is [RegionStart(), regionEnd)
            if (addr >= info->RegionStart() && addr < info->regionEnd) {
                return info;
            }
        }

        // Slow path: alignment mismatch for small/medium regions.
        // Only search ordinary small/medium adjacent entries.
        if (regionIdx > 0) {
            MSRegionInfo* prev = &array_[regionIdx - 1];
            if (prev->regionEnd != 0 &&
                !prev->IsLargeRegionEntry() &&
                addr >= prev->RegionStart() && addr < prev->regionEnd) {
                return prev;
            }
        }
        if (regionIdx + 1 < maxRegionCount_) {
            MSRegionInfo* next = &array_[regionIdx + 1];
            if (next->regionEnd != 0 &&
                !next->IsLargeRegionEntry() &&
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
        if (msInfo->IsLargeRegionEntry()) {
            MSRegionInfo* base = ResolveLargeBaseInfo(msInfo);
            // Guard: base entry already Reset (stale large region)
            if (base == nullptr) {
                return true;
            }
            return base->AtomicTestAndSetMarked();
        }
        if (msInfo->GetRegionKind() == MSRegionKind::SMALL &&
            msInfo->GetTotalSlotCount() > 0) {
            return msInfo->TestAndSetMarkBit(addr);
        }
        return true;  // uninitialized region — treat as already marked
    }

    static size_t GetMaxRegionCount() { return maxRegionCount_; }
    static void Cleanup();

private:
    static MSRegionInfo* array_;           // Array (mmap front or aligned fallback)
    static uintptr_t heapStartAddress_;    // First Region address
    static uintptr_t heapEndAddress_;      // Past-the-end address
    static size_t maxRegionCount_;
    static bool ownedByMalloc_;            // true if fallback storage must be freed
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_REGION_INFO_H
