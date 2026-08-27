// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSBitmapArena.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MSGCFastPath.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <malloc.h>
#endif
#include "Base/Log.h"

namespace MapleRuntime {

// ============================================================================
// MSRegionInfo — Lifecycle
// ============================================================================

void MSRegionInfo::InitForSizeClass(uint8_t cls, MSScanClass scanClass)
{
    uintptr_t regionStart = MSRegionInfoArray::GetRegionAddress(MSRegionInfoArray::GetRegionIdx(this));
    allocPtr = regionStart;
    regionEnd = regionStart + MS_REGION_SIZE;
    SetCentralNextRegionIdx(INVALID_REGION_IDX);
    SetMarkTaskNextRegionIdx(INVALID_REGION_IDX);
    allocBitmap = nullptr;
    collectEpoch = NO_COLLECT_EPOCH;
    control = 0;
    allocSummary = 0;
    pendingWordSummary = 0;
    reserved32 = 0;
    allocMode = static_cast<uint8_t>(MSAllocMode::BUMP);
    reserved8 = 0;
    sizeClass = cls;
    layout = static_cast<uint8_t>(MSRegionKind::SMALL);
    SetScanClass(scanClass);

    // Plane reservations are independent. NO_REF never touches the scan plane.
    size_t bitmapWords = GetBitmapWordCount();
    size_t bitmapBytes = bitmapWords * sizeof(uint64_t);
    size_t regionIdx = MSRegionInfoArray::GetRegionIdx(this);
    MSBitmapArena::EnsurePlaneRangeCommitted(MSBitmapPlane::ALLOC, regionIdx, 1);
    MSBitmapArena::EnsurePlaneRangeCommitted(MSBitmapPlane::MARK, regionIdx, 1);
    if (scanClass == MSScanClass::HAS_REF) {
        MSBitmapArena::EnsurePlaneRangeCommitted(MSBitmapPlane::SCAN, regionIdx, 1);
    }
    allocBitmap = MSBitmapArena::GetAllocPlane(regionIdx);
    uint64_t* markBitmap = MSBitmapArena::GetMarkPlane(regionIdx);
    if (allocBitmap == nullptr || markBitmap == nullptr) {
        return;
    }

    // Initialize: all slots free (allocBitmap all-1s), mark bits cleared
    memset(allocBitmap, 0xFF, bitmapBytes);
    memset(markBitmap, 0, bitmapBytes);

    allocBitmap[bitmapWords - 1] = GetValidTailMask();

    // Build summary word (O4): all bitmap words have free slots
    allocSummary = (bitmapWords == 64) ? ~0ULL : ((1ULL << bitmapWords) - 1);
}

void MSRegionInfo::InitForLargeObject(
    uintptr_t start, uintptr_t end, size_t objectBytes, MSScanClass scanClass)
{
    allocPtr = start;
    regionEnd = end;
    SetCentralNextRegionIdx(INVALID_REGION_IDX);
    SetMarkTaskNextRegionIdx(INVALID_REGION_IDX);
    largeObjectBytes = static_cast<uint64_t>(objectBytes);
    collectEpoch = NO_COLLECT_EPOCH;
    control = 0;
    allocSummary = 0;
    pendingWordSummary = 0;
    reserved32 = 0;
    allocMode = static_cast<uint8_t>(MSAllocMode::NONE);
    reserved8 = 0;
    sizeClass = INVALID_SIZE_CLASS;
    layout = static_cast<uint8_t>(MSRegionKind::LARGE);
    SetScanClass(scanClass);
}

void MSRegionInfo::MaterializeBumpAllocationBitmap()
{
    CHECK_DETAIL(GetRegionKind() == MSRegionKind::SMALL &&
                     GetAllocMode() == MSAllocMode::BUMP,
                 "MaterializeBumpAllocationBitmap requires BUMP region, type=%u",
                 static_cast<unsigned>(GetRegionType()));

    uint64_t* allocBitmap = GetAllocBitmap();
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    uintptr_t regionStart = RegionStart();
    CHECK_DETAIL(slotSize != 0 && allocPtr >= regionStart && allocPtr <= regionEnd,
                 "MaterializeBumpAllocationBitmap invalid bump range: allocPtr=0x%zx "
                 "region=[0x%zx,0x%zx) slotSize=%zu",
                 allocPtr, regionStart, regionEnd, slotSize);

    size_t allocatedBytes = allocPtr - regionStart;
    CHECK_DETAIL(allocatedBytes % slotSize == 0,
                 "MaterializeBumpAllocationBitmap unaligned allocPtr: allocatedBytes=%zu slotSize=%zu",
                 allocatedBytes, slotSize);
    size_t allocatedSlots = allocatedBytes / slotSize;
    size_t totalSlotCount = GetTotalSlotCount();
    uint16_t freeSlots = DeriveBumpFreeSlots();
    CHECK_DETAIL(allocatedSlots <= totalSlotCount &&
                     allocatedSlots + freeSlots == totalSlotCount,
                 "MaterializeBumpAllocationBitmap inconsistent slots: allocated=%zu "
                 "derivedFree=%u total=%u",
                 allocatedSlots,
                 static_cast<unsigned>(freeSlots),
                 static_cast<unsigned>(totalSlotCount));

    size_t wordCount = GetBitmapWordCount();
    CHECK_DETAIL(wordCount <= 64,
                 "MaterializeBumpAllocationBitmap wordCount overflow: wordCount=%zu",
                 wordCount);

    uint64_t summary = 0;
    for (size_t wordIdx = 0; wordIdx < wordCount; ++wordIdx) {
        size_t wordStartSlot = wordIdx * 64;
        size_t validBitCount = std::min<size_t>(64, totalSlotCount - wordStartSlot);
        uint64_t validMask = validBitCount == 64 ? ~0ULL : ((1ULL << validBitCount) - 1);
        size_t allocatedInWord = allocatedSlots > wordStartSlot
            ? std::min<size_t>(64, allocatedSlots - wordStartSlot)
            : 0;
        uint64_t allocatedMask = allocatedInWord == 64
            ? ~0ULL
            : ((1ULL << allocatedInWord) - 1);
        uint64_t freeBits = validMask & ~allocatedMask;
        allocBitmap[wordIdx] = freeBits;
        if (freeBits != 0) {
            summary |= (1ULL << wordIdx);
        }
    }
    allocSummary = summary;
    SetAllocMode(MSAllocMode::BITMAP);
}

void MSRegionInfo::ReleaseBitmaps()
{
    // Bitmap storage is indexed by Region and owned by MSBitmapArena. Resetting
    // a Region clears its non-owning alloc-plane pointer but does not release
    // the arena reservation.
}

void MSRegionInfo::Reset()
{
    ReleaseBitmaps();
    allocPtr = 0;
    regionEnd = 0;
    SetCentralNextRegionIdx(INVALID_REGION_IDX);
    SetMarkTaskNextRegionIdx(INVALID_REGION_IDX);
    allocBitmap = nullptr;
    collectEpoch = NO_COLLECT_EPOCH;
    control = 0;
    allocSummary = 0;
    pendingWordSummary = 0;
    reserved32 = 0;
    allocMode = static_cast<uint8_t>(MSAllocMode::NONE);
    reserved8 = 0;
    sizeClass = INVALID_SIZE_CLASS;
    layout = static_cast<uint8_t>(MSRegionKind::FREE);
}

void MSRegionInfo::ResetToBump()
{
    // Reset a completely-free region back to BUMP for fast reuse (O3).
    // The alloc plane is latent in BUMP mode and will be materialized from
    // allocPtr if this Region later returns non-empty.
    allocPtr = RegionStart();
    SetRegionKind(MSRegionKind::SMALL);
    SetAllocMode(MSAllocMode::BUMP);
    MSScanClass scanClass = GetScanClass();
    control = 0;
    SetScanClass(scanClass);
    allocSummary = 0;
}

void MSRegionInfo::ResetToEmptyCache()
{
    ReleaseBitmaps();
    allocPtr = 0;
    regionEnd = 0;
    SetCentralNextRegionIdx(INVALID_REGION_IDX);
    SetMarkTaskNextRegionIdx(INVALID_REGION_IDX);
    allocBitmap = nullptr;
    collectEpoch = NO_COLLECT_EPOCH;
    control = 0;
    allocSummary = 0;
    pendingWordSummary = 0;
    reserved32 = 0;
    allocMode = static_cast<uint8_t>(MSAllocMode::NONE);
    reserved8 = 0;
    sizeClass = INVALID_SIZE_CLASS;
    layout = static_cast<uint8_t>(MSRegionKind::FREE);
}

// ============================================================================
// MSRegionInfo — Sweep
// ============================================================================

static bool HasAnyValidMarkBits(
    const uint64_t* markBitmap, size_t wordCount, uint64_t validTailMask)
{
    if (wordCount == 0) {
        return false;
    }
    for (size_t i = 0; i + 1 < wordCount; ++i) {
        if (markBitmap[i] != 0) {
            return true;
        }
    }
    return (markBitmap[wordCount - 1] & validTailMask) != 0;
}

MSCentralReturnDecision MSRegionInfo::SweepRegion()
{
    const size_t totalSlotCount = GetTotalSlotCount();
    const size_t wordCount = GetBitmapWordCount();
    uint64_t* allocBitmap = GetAllocBitmap();
    uint64_t* markBitmap = GetMarkBitmap();
    CHECK_DETAIL(wordCount <= MSBitmapArena::kMaxBitmapWords,
                 "[MS-GC] SweepRegion wordCount overflow: wordCount=%zu", wordCount);
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    uintptr_t base = RegionStart();
    if (!HasAnyValidMarkBits(markBitmap, wordCount, GetValidTailMask())) {
        memset(reinterpret_cast<void*>(base), 0, MS_REGION_SIZE);
        SetAllocMode(MSAllocMode::BITMAP);
        allocSummary = 0;
        return BuildCentralReturnDecision(
            sizeClass, static_cast<uint16_t>(totalSlotCount));
    }

    size_t trailingBits = totalSlotCount % 64;
    size_t liveCount = 0;
    uint64_t summary = 0;
    size_t runStartSlot = totalSlotCount;
    auto clearDeadRun = [base, slotSize](size_t startSlot, size_t endSlot) {
        memset(reinterpret_cast<void*>(base + startSlot * slotSize),
               0,
               (endSlot - startSlot) * slotSize);
    };

    for (size_t i = 0; i < wordCount; i++) {
        // An acquire Pop from the unswept stack gives one worker exclusive
        // ownership. The Region is not published to mutators until this method
        // has rebuilt the entire allocation plane.
        size_t validBitCount = (i + 1 == wordCount && trailingBits != 0) ? trailingBits : 64;
        uint64_t validMask = validBitCount == 64 ? ~0ULL : ((1ULL << validBitCount) - 1);
        uint64_t validMarkBits = markBitmap[i] & validMask;
        liveCount += static_cast<size_t>(__builtin_popcountll(validMarkBits));
        uint64_t freeBits = ~validMarkBits & validMask;
        allocBitmap[i] = freeBits;
        if (freeBits != 0) {
            summary |= (1ULL << i);
        }

        size_t wordStartSlot = i * 64;
        if (runStartSlot != totalSlotCount && (freeBits & 1ULL) == 0) {
            clearDeadRun(runStartSlot, wordStartSlot);
            runStartSlot = totalSlotCount;
        }

        size_t bit = 0;
        while (bit < validBitCount) {
            uint64_t remainingFreeBits = freeBits >> bit;
            if (remainingFreeBits == 0) {
                break;
            }

            size_t firstFreeBit = bit + static_cast<size_t>(__builtin_ctzll(remainingFreeBits));
            if (runStartSlot == totalSlotCount) {
                runStartSlot = wordStartSlot + firstFreeBit;
            }

            uint64_t runBits = freeBits >> firstFreeBit;
            size_t runLength = runBits == ~0ULL ? 64 :
                static_cast<size_t>(__builtin_ctzll(~runBits));
            size_t validRemaining = validBitCount - firstFreeBit;
            if (runLength > validRemaining) {
                runLength = validRemaining;
            }
            size_t runEndBit = firstFreeBit + runLength;
            if (runEndBit == validBitCount) {
                break;
            }

            clearDeadRun(runStartSlot, wordStartSlot + runEndBit);
            runStartSlot = totalSlotCount;
            bit = runEndBit + 1;
        }
    }

    if (runStartSlot != totalSlotCount) {
        clearDeadRun(runStartSlot, totalSlotCount);
    }

    // All dead runs are zeroed before the release Push publishes the Region
    // back to a swept head. A run left open at a word boundary naturally joins
    // a free prefix in the following bitmap word.

    // markBitmap is cleared by ClearAllMarkBitmaps at next MarkSetup,
    // not here — avoids redundant memset per-region during sweep.

    SetAllocMode(MSAllocMode::BITMAP);

    // Store summary word (O4): bit[i]=1 if allocBitmap[i] has free slots
    allocSummary = summary;

    return BuildCentralReturnDecision(
        sizeClass, static_cast<uint16_t>(totalSlotCount - liveCount));
}

// ============================================================================
// MSRegionInfo — Mark
// ============================================================================

bool MSRegionInfo::GetMarkBit(uintptr_t objAddr) const
{
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    size_t slotIdx = (objAddr - RegionStart()) / slotSize;
    size_t wordIdx = slotIdx / 64;
    size_t bitIdx = slotIdx % 64;
    uint64_t* markBitmap = GetMarkBitmap();
    uint64_t word = __atomic_load_n(&markBitmap[wordIdx], __ATOMIC_RELAXED);
    return (word & (1ULL << bitIdx)) != 0;
}

bool MSRegionInfo::IsMarkBitSet(size_t wordIdx, uint64_t bitMask) const
{
    size_t bitmapWords = GetBitmapWordCount();
    if (wordIdx >= bitmapWords || bitMask == 0) {
        return false;
    }
    return IsMarkBitSetResolved(wordIdx, bitMask);
}

bool MSRegionInfo::IsMarkBitSetResolved(size_t wordIdx, uint64_t bitMask) const
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    size_t bitmapWords = GetBitmapWordCount();
    MRT_ASSERT(wordIdx < bitmapWords && bitMask != 0,
               "invalid resolved mark-bitmap coordinate");
#endif
    uint64_t* markBitmap = GetMarkBitmap();
    uint64_t word = __atomic_load_n(&markBitmap[wordIdx], __ATOMIC_RELAXED);
    return (word & bitMask) != 0;
}

bool MSRegionInfo::TestAndSetMarkBit(uintptr_t objAddr)
{
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    if (slotSize == 0) {
        LOG(RTLOG_ERROR, "[MS-GC] TestAndSetMarkBit: slotSize=0 for sizeClass=%u, addr=%p",
            (unsigned)sizeClass, (void*)objAddr);
        return false;
    }
    uintptr_t regionStart = RegionStart();
    if (objAddr < regionStart || objAddr >= regionEnd) {
        LOG(RTLOG_ERROR, "[MS-GC] TestAndSetMarkBit: addr=%p OUT OF RANGE [%p, %p), "
            "sizeClass=%u, regionType=%u, markBitmap=%p",
            (void*)objAddr, (void*)regionStart, (void*)regionEnd,
            (unsigned)sizeClass, (unsigned)GetRegionType(), (void*)GetMarkBitmap());
        return false;
    }
    // Division-free slot index via reciprocal magic (R-2). Exact because
    // (objAddr - regionStart) < MS_REGION_SIZE (2^15). See MSSizeClass.h.
    size_t slotIdx = SizeClassInfo::OffsetToSlotIndex(sizeClass, objAddr - regionStart);
    if (slotIdx >= GetTotalSlotCount()) {
        LOG(RTLOG_ERROR, "[MS-GC] TestAndSetMarkBit: slotIdx=%zu >= totalSlots=%u, addr=%p, "
            "regionStart=%p, slotSize=%zu",
            slotIdx, GetTotalSlotCount(), (void*)objAddr, (void*)regionStart, slotSize);
        return false;
    }
    size_t wordIdx = slotIdx / 64;
    uint64_t bit = 1ULL << (slotIdx % 64);
    uint64_t* markBitmap = GetMarkBitmap();
    uint64_t old = __atomic_fetch_or(&markBitmap[wordIdx], bit, __ATOMIC_RELAXED);
    return (old & bit) != 0;  // true if was already marked
}

uint64_t MSRegionInfo::MarkBitmapWord(size_t wordIdx, uint64_t mask)
{
    size_t bitmapWords = GetBitmapWordCount();
    if (wordIdx >= bitmapWords || mask == 0) {
        return 0;
    }
    return MarkBitmapWordResolved(wordIdx, mask);
}

uint64_t MSRegionInfo::MarkBitmapWordResolved(size_t wordIdx, uint64_t mask)
{
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    size_t bitmapWords = GetBitmapWordCount();
    MRT_ASSERT(wordIdx < bitmapWords && mask != 0,
               "invalid resolved mark-bitmap coordinate");
#endif
    uint64_t* markBitmap = GetMarkBitmap();
    uint64_t old = __atomic_fetch_or(&markBitmap[wordIdx], mask, __ATOMIC_RELAXED);
    return mask & ~old;
}

// ============================================================================
// MSRegionInfo — Queries
// ============================================================================

uint16_t MSRegionInfo::DeriveBumpFreeSlots() const
{
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    uintptr_t regionStart = RegionStart();
    CHECK_DETAIL(slotSize != 0 && allocPtr >= regionStart && allocPtr <= regionEnd,
                 "DeriveBumpFreeSlots invalid bump range: allocPtr=0x%zx "
                 "region=[0x%zx,0x%zx) slotSize=%zu",
                 allocPtr, regionStart, regionEnd, slotSize);
    CHECK_DETAIL((allocPtr - regionStart) % slotSize == 0,
                 "DeriveBumpFreeSlots unaligned allocPtr: allocPtr=0x%zx "
                 "regionStart=0x%zx slotSize=%zu",
                 allocPtr, regionStart, slotSize);
    size_t freeSlots = (regionEnd - allocPtr) / slotSize;
    CHECK_DETAIL(freeSlots <= GetTotalSlotCount(),
                 "DeriveBumpFreeSlots overflow: free=%zu total=%u",
                 freeSlots, static_cast<unsigned>(GetTotalSlotCount()));
    return static_cast<uint16_t>(freeSlots);
}

uint16_t MSRegionInfo::CanonicalizeBitmapFreeSlots()
{
    uint64_t* bitmap = GetAllocBitmap();
    size_t wordCount = GetBitmapWordCount();
    CHECK_DETAIL(bitmap != nullptr && wordCount > 0 &&
                     wordCount <= MSBitmapArena::kMaxBitmapWords,
                 "CanonicalizeBitmapFreeSlots invalid bitmap: bitmap=%p words=%zu",
                 static_cast<void*>(bitmap), wordCount);

    uint64_t summary = 0;
    size_t freeSlots = 0;
    for (size_t wordIdx = 0; wordIdx < wordCount; ++wordIdx) {
        uint64_t validMask =
            wordIdx + 1 == wordCount ? GetValidTailMask() : ~UINT64_C(0);
        uint64_t freeBits = bitmap[wordIdx] & validMask;
        bitmap[wordIdx] = freeBits;
        freeSlots += static_cast<size_t>(__builtin_popcountll(freeBits));
        if (freeBits != 0) {
            summary |= UINT64_C(1) << wordIdx;
        }
    }

    CHECK_DETAIL(freeSlots <= GetTotalSlotCount(),
                 "CanonicalizeBitmapFreeSlots overflow: free=%zu total=%u",
                 freeSlots, static_cast<unsigned>(GetTotalSlotCount()));
    allocSummary = summary;
    return static_cast<uint16_t>(freeSlots);
}

uint16_t MSRegionInfo::CountFreeSlots() const
{
    if (GetRegionKind() != MSRegionKind::SMALL ||
        sizeClass >= kNumSizeClasses) {
        return 0;
    }
    if (GetAllocMode() == MSAllocMode::BUMP) {
        return DeriveBumpFreeSlots();
    }
    if (GetAllocMode() != MSAllocMode::BITMAP ||
        GetAllocBitmap() == nullptr) {
        return 0;
    }

    size_t freeSlots = 0;
    size_t wordCount = GetBitmapWordCount();
    for (size_t wordIdx = 0; wordIdx < wordCount; ++wordIdx) {
        uint64_t validMask =
            wordIdx + 1 == wordCount ? GetValidTailMask() : ~UINT64_C(0);
        freeSlots += static_cast<size_t>(
            __builtin_popcountll(GetAllocBitmap()[wordIdx] & validMask));
    }
    return static_cast<uint16_t>(freeSlots);
}

MSCentralReturnDecision MSRegionInfo::BuildCentralReturnDecision(
    uint8_t callerCls, uint16_t freeSlots) const
{
    MSCentralReturnKind kind = MSCentralReturnKind::FULL;
    if (freeSlots == GetTotalSlotCount()) {
        kind = MSCentralReturnKind::EMPTY_CACHE;
    } else if (freeSlots != 0) {
        kind = MSCentralReturnKind::PARTIAL;
    }
    return {
        kind,
        sizeClass,
        callerCls != sizeClass,
        freeSlots,
    };
}

MSCentralReturnDecision MSRegionInfo::PrepareForCentralReturn(uint8_t callerCls)
{
    CHECK_DETAIL(GetRegionKind() == MSRegionKind::SMALL,
                 "PrepareForCentralReturn requires SMALL region, kind=%u",
                 static_cast<unsigned>(GetRegionKind()));

    uint16_t freeSlots = 0;
    if (GetAllocMode() == MSAllocMode::BUMP) {
        freeSlots = DeriveBumpFreeSlots();
        if (freeSlots != GetTotalSlotCount()) {
            // Mark treats ALLOC bit 1 as a free slot. Publish an exact bitmap
            // before a non-empty BUMP Region becomes Central-visible.
            MaterializeBumpAllocationBitmap();
        }
    } else {
        CHECK_DETAIL(GetAllocMode() == MSAllocMode::BITMAP,
                     "PrepareForCentralReturn invalid allocMode=%u",
                     static_cast<unsigned>(GetAllocMode()));
        // allocSummary is a lookup hint and may be stale in either direction.
        // The cold return boundary canonicalizes it from the source bitmap.
        freeSlots = CanonicalizeBitmapFreeSlots();
    }
    return BuildCentralReturnDecision(callerCls, freeSlots);
}

// ============================================================================
// MSRegionInfoArray — Mirror array
// ============================================================================

MSRegionInfo* MSRegionInfoArray::array_ = nullptr;
uintptr_t MSRegionInfoArray::heapStartAddress_ = 0;
uintptr_t MSRegionInfoArray::heapEndAddress_ = 0;
size_t MSRegionInfoArray::maxRegionCount_ = 0;

void MSRegionInfoArray::Init(uintptr_t heapStart, size_t maxRegionCount, MSRegionInfo* array)
{
    PublishMSGCFastPathRegionInfoArray(0);
    heapStartAddress_ = heapStart;
    maxRegionCount_ = maxRegionCount;
    heapEndAddress_ = heapStart + maxRegionCount * MS_REGION_SIZE;

    size_t arrayBytes = maxRegionCount * sizeof(MSRegionInfo);

    if (array != nullptr) {
        // Use externally provided array (from mmap front, managed by MSRegionManager).
        array_ = array;
        ownedByMalloc_ = false;
        // MSRegionManager::Initialize already zero'd and set regionType/indices.
        LOG(RTLOG_INFO, "[MS-GC] MSRegionInfoArray::Init: heapStart=0x%zx, maxRegions=%zu, "
            "arraySize=%zuKB (mmap front)",
            heapStart, maxRegionCount, arrayBytes / 1024);
    } else {
        // Standalone/unit-test fallback. MSRegionInfo is over-aligned, so plain
        // malloc is not a valid C++14 allocation strategy on every platform.
#ifdef _WIN32
        array_ = static_cast<MSRegionInfo*>(
            _aligned_malloc(arrayBytes, alignof(MSRegionInfo)));
#else
        void* alignedArray = nullptr;
        int allocResult = posix_memalign(
            &alignedArray, alignof(MSRegionInfo), arrayBytes);
        array_ = allocResult == 0 ? static_cast<MSRegionInfo*>(alignedArray) : nullptr;
#endif
        ownedByMalloc_ = true;
        if (array_ == nullptr) {
            LOG(RTLOG_ERROR, "[MS-GC] MSRegionInfoArray::Init: failed to allocate %zuKB",
                arrayBytes / 1024);
            return;
        }
        memset(array_, 0, arrayBytes);
        for (size_t i = 0; i < maxRegionCount; i++) {
            array_[i].Reset();
        }
        LOG(RTLOG_INFO, "[MS-GC] MSRegionInfoArray::Init: heapStart=0x%zx, maxRegions=%zu, "
            "arraySize=%zuKB (aligned fallback)",
            heapStart, maxRegionCount, arrayBytes / 1024);
    }

    PublishMSGCFastPathRegionInfoArray(reinterpret_cast<uintptr_t>(array_));
}

bool MSRegionInfoArray::ownedByMalloc_ = false;

void MSRegionInfoArray::Cleanup()
{
    PublishMSGCFastPathRegionInfoArray(0);
    if (array_ != nullptr) {
        if (ownedByMalloc_) {
#ifdef MRT_WINDOWS
            _aligned_free(array_);
#else
            free(array_);
#endif
        }
        array_ = nullptr;
    }
    ownedByMalloc_ = false;
    heapStartAddress_ = 0;
    heapEndAddress_ = 0;
    maxRegionCount_ = 0;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
