// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Allocator/MSRegionInfo.h"

#include <cstdlib>
#ifndef _WIN64
#include <sys/mman.h>
#endif

#include "Base/Log.h"

namespace MapleRuntime {

// ============================================================================
// MSRegionInfo — Lifecycle
// ============================================================================

void MSRegionInfo::InitForSizeClass(uint8_t cls)
{
    sizeClass = cls;
    sweepState = MSSweepState::UNSWEPT;
    regionType = MSRegionType::MS_REGION_BUMP;

    size_t slotSize = SizeClassInfo::ClassToSize(cls);
    totalSlotCount = static_cast<uint16_t>(MS_REGION_SIZE / slotSize);
    freeSlotCount = totalSlotCount;
    liveSlotCount = 0;
    rawPointerObjCount = 0;

    isMarked = 0;
    isEnqueued = 0;
    isResurrected = 0;
    flagsPadding = 0;
    sweepGen = 0;
    reserved = 0;  // searchHint = 0

    // Allocate allocBitmap + markBitmap contiguously via mmap
    size_t bitmapWords = (totalSlotCount + 63) / 64;
    size_t bitmapBytes = bitmapWords * sizeof(uint64_t);
    size_t totalBytes = bitmapBytes * 2;

#ifndef _WIN64
    void* mem = mmap(nullptr, totalBytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        allocBitmap = nullptr;
        markBitmap = nullptr;
        return;
    }
#else
    void* mem = VirtualAlloc(nullptr, totalBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (mem == nullptr) {
        allocBitmap = nullptr;
        markBitmap = nullptr;
        return;
    }
#endif

    allocBitmap = reinterpret_cast<uint64_t*>(mem);
    markBitmap = reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(mem) + bitmapBytes);

    // Initialize: all slots free (allocBitmap all-1s), mark bits cleared
    memset(allocBitmap, 0xFF, bitmapBytes);
    memset(markBitmap, 0, bitmapBytes);

    // Mask out invalid trailing bits in the last word if totalSlotCount % 64 != 0
    size_t trailingBits = totalSlotCount % 64;
    if (trailingBits != 0) {
        allocBitmap[bitmapWords - 1] = (1ULL << trailingBits) - 1;
    }
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

    isMarked = 0;
    isEnqueued = 0;
    isResurrected = 0;
    flagsPadding = 0;
    sweepGen = 0;
    reserved = 0;
}

void MSRegionInfo::ReleaseBitmaps()
{
    if (allocBitmap != nullptr) {
        size_t bitmapWords = (totalSlotCount + 63) / 64;
        size_t bitmapBytes = bitmapWords * sizeof(uint64_t);
        size_t totalBytes = bitmapBytes * 2;
#ifndef _WIN64
        munmap(allocBitmap, totalBytes);
#else
        VirtualFree(allocBitmap, 0, MEM_RELEASE);
#endif
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
    isMarked = 0;
    isEnqueued = 0;
    isResurrected = 0;
    flagsPadding = 0;
    sweepGen = 0;
    reserved = 0;
}

// ============================================================================
// MSRegionInfo — Allocation
// ============================================================================

uintptr_t MSRegionInfo::Alloc()
{
    // Phase 2: after sweep, use bitmap allocation
    if (sweepState == MSSweepState::SWEPT) {
        return AllocFromBitmap();
    }

    // Phase 1: bump-pointer for fresh Region (BUMP state)
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    if (allocPtr + slotSize <= regionEnd) {
        uintptr_t addr = allocPtr;
        allocPtr += slotSize;
        freeSlotCount--;

        // Sync allocBitmap: mark slot as allocated (clear bit)
        uintptr_t start = RegionStart();
        size_t slotIdx = (addr - start) / slotSize;
        allocBitmap[slotIdx / 64] &= ~(1ULL << (slotIdx % 64));

        return addr;
    }

    return 0;  // Region full
}

uintptr_t MSRegionInfo::AllocFromBitmap()
{
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    size_t wordCount = (totalSlotCount + 63) / 64;

    // Use searchHint (stored in 'reserved' field, lower 32 bits) to skip empty words
    size_t hint = static_cast<size_t>(reserved & 0xFFFFFFFF);
    for (size_t i = hint; i < wordCount; i++) {
        if (allocBitmap[i] != 0) {
            int bit = __builtin_ctzll(allocBitmap[i]);
            allocBitmap[i] &= ~(1ULL << bit);  // Clear bit = mark as allocated
            freeSlotCount--;

            // Update searchHint
            reserved = (reserved & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(i);

            size_t slotIndex = i * 64 + static_cast<size_t>(bit);
            uintptr_t addr = RegionStart() + slotIndex * slotSize;
            return addr;
        }
    }

    // Wrap around: search from 0 to hint
    for (size_t i = 0; i < hint; i++) {
        if (allocBitmap[i] != 0) {
            int bit = __builtin_ctzll(allocBitmap[i]);
            allocBitmap[i] &= ~(1ULL << bit);
            freeSlotCount--;

            reserved = (reserved & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(i);

            size_t slotIndex = i * 64 + static_cast<size_t>(bit);
            return RegionStart() + slotIndex * slotSize;
        }
    }

    return 0;  // Region completely full
}

// ============================================================================
// MSRegionInfo — Sweep
// ============================================================================

size_t MSRegionInfo::SweepRegion()
{
    size_t wordCount = (totalSlotCount + 63) / 64;
    int liveCount = 0;

    for (size_t i = 0; i < wordCount; i++) {
        // Core: allocBitmap = ~markBitmap  (one NOT per 64 slots)
        allocBitmap[i] = ~markBitmap[i];
        liveCount += __builtin_popcountll(markBitmap[i]);
    }

    // Mask out invalid trailing bits
    size_t trailingBits = totalSlotCount % 64;
    if (trailingBits != 0) {
        allocBitmap[wordCount - 1] &= (1ULL << trailingBits) - 1;
    }

    // Clear markBitmap for next GC cycle
    memset(markBitmap, 0, wordCount * sizeof(uint64_t));

    liveSlotCount = static_cast<uint16_t>(liveCount);
    freeSlotCount = totalSlotCount - liveSlotCount;
    sweepState = MSSweepState::SWEPT;

    // Reset searchHint to 0
    reserved = (reserved & 0xFFFFFFFF00000000ULL);

    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
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
