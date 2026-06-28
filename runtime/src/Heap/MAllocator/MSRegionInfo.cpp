// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSBitmapArena.h"
#include "Heap/MAllocator/MSRegionInfo.h"

#include <cstring>
#include "Base/Log.h"

namespace MapleRuntime {

// ============================================================================
// MSRegionInfo — Lifecycle
// ============================================================================

void MSRegionInfo::InitForSizeClass(uint8_t cls)
{
    uintptr_t regionStart = MSRegionInfoArray::GetRegionAddress(MSRegionInfoArray::GetRegionIdx(this));
    allocPtr = regionStart;
    regionEnd = regionStart + MS_REGION_SIZE;
    sizeClass = cls;
    sweepState = MSSweepState::UNSWEPT;
    regionType = MSRegionType::MS_REGION_BUMP;

    size_t slotSize = SizeClassInfo::ClassToSize(cls);
    totalSlotCount = static_cast<uint16_t>(MS_REGION_SIZE / slotSize);
    freeSlotCount = totalSlotCount;
    liveSlotCount = 0;
    rawPointerObjCount = 0;

    flags_ = 0;
    sweepGen = 0;
    reserved = 0;  // searchHint = 0

    // Use the fixed sidecar slot for this region:
    // [allocBitmap | markBitmap].
    size_t bitmapWords = (totalSlotCount + 63) / 64;
    size_t bitmapBytes = bitmapWords * sizeof(uint64_t);
    size_t regionIdx = MSRegionInfoArray::GetRegionIdx(this);
    uint64_t* mem = MSBitmapArena::GetSlotBase(regionIdx);
    if (mem == nullptr) {
        allocBitmap = nullptr;
        markBitmap = nullptr;
        return;
    }

    allocBitmap = mem;
    markBitmap = reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(mem) + bitmapBytes);

    // Initialize: all slots free (allocBitmap all-1s), mark bits cleared
    memset(allocBitmap, 0xFF, bitmapBytes);
    memset(markBitmap, 0, bitmapBytes);

    // Mask out invalid trailing bits in the last word if totalSlotCount % 64 != 0
    size_t trailingBits = totalSlotCount % 64;
    if (trailingBits != 0) {
        allocBitmap[bitmapWords - 1] = (1ULL << trailingBits) - 1;
    }

    // Build summary word (O4): all bitmap words have free slots
    reserved = (bitmapWords == 64) ? ~0ULL : ((1ULL << bitmapWords) - 1);
}

void MSRegionInfo::InitForLargeObject(uintptr_t start, uintptr_t end)
{
    allocPtr = start;
    regionEnd = end;
    nextRegionIdx = INVALID_IDX;
    prevRegionIdx = INVALID_IDX;

    allocBitmap = nullptr;
    markBitmap = nullptr;

    totalSlotCount = 1;
    freeSlotCount = 0;
    liveSlotCount = 1;
    rawPointerObjCount = 0;

    sizeClass = 0;  // N/A for large objects
    sweepState = MSSweepState::UNSWEPT;
    regionType = MSRegionType::MS_REGION_LARGE;

    flags_ = 0;
    sweepGen = 0;
    reserved = 0;
}

void MSRegionInfo::ReleaseBitmaps()
{
    if (allocBitmap != nullptr) {
        allocBitmap = nullptr;
        markBitmap = nullptr;
    }
}

void MSRegionInfo::Reset()
{
    ReleaseBitmaps();
    allocPtr = 0;
    regionEnd = 0;
    nextRegionIdx = INVALID_IDX;
    prevRegionIdx = INVALID_IDX;
    totalSlotCount = 0;
    freeSlotCount = 0;
    liveSlotCount = 0;
    rawPointerObjCount = 0;
    sizeClass = 0;
    sweepState = MSSweepState::UNSWEPT;
    regionType = MSRegionType::MS_REGION_FREE;
    flags_ = 0;
    sweepGen = 0;
    reserved = 0;
}

void MSRegionInfo::ResetToBump()
{
    // Reset a completely-free region back to BUMP for fast reuse (O3).
    // Preserves bitmap memory — avoids munmap/mmap cycle.
    bool sweptEmpty = sweepState == MSSweepState::SWEPT &&
                      freeSlotCount == totalSlotCount &&
                      liveSlotCount == 0;
    allocPtr = RegionStart();
    sweepState = MSSweepState::UNSWEPT;
    regionType = MSRegionType::MS_REGION_BUMP;
    freeSlotCount = totalSlotCount;
    liveSlotCount = 0;
    rawPointerObjCount = 0;
    flags_ = 0;
    if (sweptEmpty) {
        // SweepRegion() already produced a fully-zero payload and a clear mark bitmap.
        // Bump allocation does not consult allocBitmap/reserved, so avoid rebuilding
        // the swept bitmap state when lazy sweep immediately hands this region back
        // to the mutator as a fresh BUMP region.
        reserved = 0;
        return;
    }

    // Reset allocBitmap to all-1s (all slots free)
    size_t bitmapWords = (totalSlotCount + 63) / 64;
    memset(allocBitmap, 0xFF, bitmapWords * sizeof(uint64_t));
    size_t trailingBits = totalSlotCount % 64;
    if (trailingBits != 0) {
        allocBitmap[bitmapWords - 1] = (1ULL << trailingBits) - 1;
    }
    // Clear markBitmap for safety
    memset(markBitmap, 0, bitmapWords * sizeof(uint64_t));

    // Build summary word (O4): all bitmap words have free slots
    reserved = (bitmapWords == 64) ? ~0ULL : ((1ULL << bitmapWords) - 1);
}

void MSRegionInfo::ResetToEmptyCache()
{
    ReleaseBitmaps();
    allocPtr = 0;
    regionEnd = 0;
    nextRegionIdx = INVALID_IDX;
    prevRegionIdx = INVALID_IDX;
    totalSlotCount = 0;
    freeSlotCount = 0;
    liveSlotCount = 0;
    rawPointerObjCount = 0;
    sizeClass = INVALID_SIZE_CLASS;
    sweepState = MSSweepState::SWEPT;
    regionType = MSRegionType::MS_REGION_EMPTY_CACHE;
    flags_ = 0;
    reserved = 0;
}

// ============================================================================
// MSRegionInfo — Allocation
// ============================================================================

uintptr_t MSRegionInfo::Alloc()
{
    // Phase 2: after sweep, use bitmap allocation
    if (sweepState == MSSweepState::SWEPT) {
        return AllocFromBitmapFast();
    }

    // Phase 1: bump-pointer for fresh Region (BUMP state)
#ifdef MRT_DEBUG
    if (UNLIKELY(regionType == MSRegionType::MS_REGION_LARGE ||
                 regionType == MSRegionType::MS_REGION_FREE ||
                 regionType == MSRegionType::MS_REGION_EMPTY_CACHE)) {
        LOG(RTLOG_FATAL, "[MS-GC] Alloc() on bad regionType=%d, allocPtr=0x%zx, "
            "regionEnd=0x%zx, sizeClass=%u, allocBitmap=%p, this=%p",
            (int)regionType, allocPtr, regionEnd, (unsigned)sizeClass,
            (void*)allocBitmap, (void*)this);
    }
#endif

    return AllocBump();
}

uintptr_t MSRegionInfo::AllocFromBitmap()
{
    return AllocFromBitmapFast();
}

uintptr_t MSRegionInfo::AllocFromBitmapSlow(size_t /* hint */)
{
    // With summary word (O4), AllocFromBitmapFast handles all cases.
    // This is kept as a legacy fallback for Alloc().
    return AllocFromBitmapFast();
}

// ============================================================================
// MSRegionInfo — Sweep
// ============================================================================

size_t MSRegionInfo::SweepRegion()
{
    size_t wordCount = (totalSlotCount + 63) / 64;
    int liveCount = 0;
    uint64_t markWords[MSBitmapArena::kMaxBitmapWords] = { 0 };
    CHECK_DETAIL(wordCount <= MSBitmapArena::kMaxBitmapWords,
                 "[MS-GC] SweepRegion wordCount overflow: wordCount=%zu", wordCount);
    for (size_t i = 0; i < wordCount; i++) {
        uint64_t markWord = markBitmap[i];
        markWords[i] = markWord;
        liveCount += __builtin_popcountll(markWord);
    }

    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    uintptr_t base = RegionStart();
    if (liveCount == 0) {
        memset(reinterpret_cast<void*>(base), 0, MS_REGION_SIZE);
        liveSlotCount = 0;
        freeSlotCount = totalSlotCount;
        sweepState = MSSweepState::SWEPT;
        reserved = 0;
        return static_cast<size_t>(freeSlotCount) * slotSize;
    }

    uint64_t summary = 0;
    for (size_t i = 0; i < wordCount; i++) {
        // Sweep ownership is serialized by CasSweepGen on unswept lists. The
        // region is not returned to mutators until SweepRegion() completes, so
        // allocBitmap publication does not race with allocation on this region.
        allocBitmap[i] = ~markWords[i];
        if (allocBitmap[i] != 0) {
            summary |= (1ULL << i);
        }
    }

    size_t trailingBits = totalSlotCount % 64;
    if (trailingBits != 0) {
        allocBitmap[wordCount - 1] &= (1ULL << trailingBits) - 1;
        if (allocBitmap[wordCount - 1] == 0) {
            summary &= ~(1ULL << (wordCount - 1));
        }
    }

    // Zero dead slots before the region is put back into swept lists. This
    // keeps allocation fast-path free of per-slot clearing.
    for (size_t i = 0; i < wordCount; i++) {
        uint64_t freeBits = allocBitmap[i];
        while (freeBits != 0) {
            int bit = __builtin_ctzll(freeBits);
            size_t slotIdx = i * 64 + static_cast<size_t>(bit);
            uintptr_t slotAddr = base + slotIdx * slotSize;
            memset(reinterpret_cast<void*>(slotAddr), 0, slotSize);
            freeBits &= freeBits - 1;  // clear lowest set bit
        }
    }

    // markBitmap is cleared by ClearAllMarkBitmaps at next MarkSetup,
    // not here — avoids redundant memset per-region during sweep.

    liveSlotCount = static_cast<uint16_t>(liveCount);
    freeSlotCount = totalSlotCount - liveSlotCount;
    sweepState = MSSweepState::SWEPT;

    // Store summary word (O4): bit[i]=1 if allocBitmap[i] has free slots
    reserved = summary;

    return static_cast<size_t>(freeSlotCount) * slotSize;
}

// ============================================================================
// MSRegionInfo — Mark
// ============================================================================

void MSRegionInfo::SetMarkBit(uintptr_t objAddr)
{
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    size_t slotIdx = (objAddr - RegionStart()) / slotSize;
    size_t wordIdx = slotIdx / 64;
    size_t bitIdx = slotIdx % 64;
    // Atomic OR for concurrent marking
    __atomic_or_fetch(&markBitmap[wordIdx], 1ULL << bitIdx, __ATOMIC_RELAXED);
}

bool MSRegionInfo::GetMarkBit(uintptr_t objAddr) const
{
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    size_t slotIdx = (objAddr - RegionStart()) / slotSize;
    size_t wordIdx = slotIdx / 64;
    size_t bitIdx = slotIdx % 64;
    uint64_t word = __atomic_load_n(&markBitmap[wordIdx], __ATOMIC_RELAXED);
    return (word & (1ULL << bitIdx)) != 0;
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
            (unsigned)sizeClass, (unsigned)regionType, (void*)markBitmap);
        return false;
    }
    size_t slotIdx = (objAddr - regionStart) / slotSize;
    if (slotIdx >= totalSlotCount) {
        LOG(RTLOG_ERROR, "[MS-GC] TestAndSetMarkBit: slotIdx=%zu >= totalSlots=%u, addr=%p, "
            "regionStart=%p, slotSize=%zu",
            slotIdx, totalSlotCount, (void*)objAddr, (void*)regionStart, slotSize);
        return false;
    }
    size_t wordIdx = slotIdx / 64;
    uint64_t bit = 1ULL << (slotIdx % 64);
    uint64_t old = __atomic_fetch_or(&markBitmap[wordIdx], bit, __ATOMIC_RELAXED);
    return (old & bit) != 0;  // true if was already marked
}

// ============================================================================
// MSRegionInfo — Queries
// ============================================================================

bool MSRegionInfo::HasFreeSpace() const
{
    // After sweep, only bitmap matters — bump tail is stale
    if (sweepState == MSSweepState::SWEPT) {
        return freeSlotCount > 0;
    }
    if (freeSlotCount > 0) {
        return true;
    }
    // Bump-pointer tail space (only valid for BUMP regions before first sweep)
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    return allocPtr + slotSize <= regionEnd;
}

size_t MSRegionInfo::GetAllocatedBytes() const
{
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    return static_cast<size_t>(totalSlotCount - freeSlotCount) * slotSize;
}

size_t MSRegionInfo::GetLiveBytes() const
{
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    return static_cast<size_t>(liveSlotCount) * slotSize;
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
        // Fallback: allocate via malloc (backward compatibility).
        array_ = static_cast<MSRegionInfo*>(malloc(arrayBytes));
        ownedByMalloc_ = true;
        if (array_ == nullptr) {
            LOG(RTLOG_ERROR, "[MS-GC] MSRegionInfoArray::Init: failed to allocate %zuKB",
                arrayBytes / 1024);
            return;
        }
        memset(array_, 0, arrayBytes);
        for (size_t i = 0; i < maxRegionCount; i++) {
            array_[i].regionType = MSRegionType::MS_REGION_FREE;
            array_[i].nextRegionIdx = MSRegionInfo::INVALID_IDX;
            array_[i].prevRegionIdx = MSRegionInfo::INVALID_IDX;
        }
        LOG(RTLOG_INFO, "[MS-GC] MSRegionInfoArray::Init: heapStart=0x%zx, maxRegions=%zu, "
            "arraySize=%zuKB (malloc'd)",
            heapStart, maxRegionCount, arrayBytes / 1024);
    }
}

bool MSRegionInfoArray::ownedByMalloc_ = false;

void MSRegionInfoArray::Cleanup()
{
    if (array_ != nullptr) {
        if (ownedByMalloc_) {
            free(array_);
        }
        array_ = nullptr;
    }
    ownedByMalloc_ = false;
    maxRegionCount_ = 0;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
