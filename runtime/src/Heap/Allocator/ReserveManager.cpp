// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Allocator/ReserveManager.h"

#include "Base/LogFile.h"

namespace MapleRuntime {

void ReserveManager::Initialize(uintptr_t start, uintptr_t end, size_t unitSz)
{
    reserveStart = start;
    reserveEnd = end;
    unitSize = unitSz;

    // Initialize watermarks
    mainWatermark.store(reserveStart, std::memory_order_relaxed);
    losWatermark.store(reserveEnd, std::memory_order_relaxed);

    // Initialize hole bitmap (1 bit per RESERVE unit, all zero)
    totalReserveUnits = (reserveEnd - reserveStart) / unitSize;

    DLOG(REPORT, "ReserveManager initialized: zone [0x%zx, 0x%zx), capacity %zu bytes, "
         "%zu units",
         reserveStart, reserveEnd, GetCapacity(), totalReserveUnits);
}

size_t ReserveManager::GetAvailable() const
{
    uintptr_t mw = mainWatermark.load(std::memory_order_relaxed);
    uintptr_t lw = losWatermark.load(std::memory_order_relaxed);
    return (lw > mw) ? (lw - mw) : 0;
}

// ===== A+B hybrid chunk strategy =====

size_t ReserveManager::ComputeBorrowChunk(size_t requestSize) const
{
    size_t available = GetAvailable();
    size_t minChunk = MIN_BORROW_CHUNK_UNITS * unitSize;
    size_t proportional = available / BORROW_RATIO_DIVISOR;

    // target = max(requestSize, max(minChunk, proportional))
    size_t target = requestSize;
    if (minChunk > target) {
        target = minChunk;
    }
    if (proportional > target) {
        target = proportional;
    }

    // Align up to chunkAlignment
    size_t alignment = BORROW_CHUNK_ALIGN_UNITS * unitSize;
    if (alignment > 0) {
        target = (target + alignment - 1) / alignment * alignment;
    }

    // Cap at available — if too large, fall back to exact request (unit-aligned)
    if (target > available) {
        target = (requestSize + unitSize - 1) / unitSize * unitSize;
    }

    DLOG(REGION, "ReserveManager: ComputeBorrowChunk request=%zu, available=%zu, "
         "minChunk=%zu, proportional=%zu, result=%zu",
         requestSize, available, minChunk, proportional, target);
    return target;
}

uintptr_t ReserveManager::CommitChunkForMain(size_t chunkSize)
{
    // Caller holds borrowMutex
    uintptr_t curMain = mainWatermark.load(std::memory_order_relaxed);
    uintptr_t curLOS = losWatermark.load(std::memory_order_relaxed);
    uintptr_t newMain = curMain + chunkSize;

    if (newMain > curLOS) {
        DLOG(REGION, "ReserveManager: CommitChunkForMain failed, requested %zu, available %zu",
             chunkSize, curLOS - curMain);
        return 0;
    }

    mainWatermark.store(newMain, std::memory_order_relaxed);
    DLOG(REGION, "ReserveManager: CommitChunkForMain %zu bytes @0x%zx, remaining %zu",
         chunkSize, curMain, curLOS - newMain);
    return curMain;
}

uintptr_t ReserveManager::CommitChunkForLOS(size_t chunkSize)
{
    // Caller holds borrowMutex
    uintptr_t curMain = mainWatermark.load(std::memory_order_relaxed);
    uintptr_t curLOS = losWatermark.load(std::memory_order_relaxed);
    uintptr_t newLOS = curLOS - chunkSize;

    if (newLOS < curMain) {
        DLOG(REGION, "ReserveManager: CommitChunkForLOS failed, requested %zu, available %zu",
             chunkSize, curLOS - curMain);
        return 0;
    }

    losWatermark.store(newLOS, std::memory_order_relaxed);
    DLOG(REGION, "ReserveManager: CommitChunkForLOS %zu bytes @0x%zx, remaining %zu",
         chunkSize, newLOS, newLOS - curMain);
    return newLOS;
}

// ===== Public borrow API (runway extension) =====

size_t ReserveManager::BorrowForMain(size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    size_t chunkSize = ComputeBorrowChunk(size);
    uintptr_t chunkAddr = CommitChunkForMain(chunkSize);
    if (chunkAddr == 0) {
        return 0;
    }

    mainChunkBorrowCount++;
    mainBorrowBytes += chunkSize;

    DLOG(REGION, "ReserveManager: BorrowForMain extended runway by %zu bytes, "
         "watermark now 0x%zx",
         chunkSize, mainWatermark.load(std::memory_order_relaxed));
    return chunkSize;
}

size_t ReserveManager::BorrowForLOS(size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    size_t chunkSize = ComputeBorrowChunk(size);
    uintptr_t chunkAddr = CommitChunkForLOS(chunkSize);
    if (chunkAddr == 0) {
        return 0;
    }

    losChunkBorrowCount++;
    losBorrowBytes += chunkSize;

    DLOG(REGION, "ReserveManager: BorrowForLOS extended runway by %zu bytes, "
         "watermark now 0x%zx",
         chunkSize, losWatermark.load(std::memory_order_relaxed));
    return chunkSize;
}

// ===== P3: Bulk reclaim — direct watermark push (no bitmap) =====

size_t ReserveManager::ReclaimMainBorrowed(uintptr_t newWatermark)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    uintptr_t currentMain = mainWatermark.load(std::memory_order_relaxed);

    // Sanity checks
    if (newWatermark >= currentMain) {
        return 0;  // nothing to reclaim (or invalid — newWatermark should be lower)
    }
    if (newWatermark < reserveStart) {
        newWatermark = reserveStart;  // cap at RESERVE boundary
    }

    size_t reclaimedBytes = currentMain - newWatermark;

    // Push watermark back (toward reserveStart)
    mainWatermark.store(newWatermark, std::memory_order_relaxed);

    // Update statistics
    mainReturnBytes += reclaimedBytes;

    DLOG(REPORT, "ReserveManager: ReclaimMainBorrowed %zu bytes, watermark 0x%zx -> 0x%zx",
         reclaimedBytes, currentMain, newWatermark);

    return reclaimedBytes;
}

size_t ReserveManager::ReclaimLOSBorrowed(uintptr_t newWatermark)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    uintptr_t currentLOS = losWatermark.load(std::memory_order_relaxed);

    // Sanity checks
    if (newWatermark <= currentLOS) {
        return 0;  // nothing to reclaim (or invalid)
    }
    if (newWatermark > reserveEnd) {
        newWatermark = reserveEnd;  // cap at RESERVE boundary
    }

    size_t reclaimedBytes = newWatermark - currentLOS;

    // Push watermark
    losWatermark.store(newWatermark, std::memory_order_relaxed);

    // Update statistics
    losReturnBytes += reclaimedBytes;

    DLOG(REPORT, "ReserveManager: ReclaimLOSBorrowed %zu bytes, watermark 0x%zx -> 0x%zx",
         reclaimedBytes, currentLOS, newWatermark);

    return reclaimedBytes;
}

size_t ReserveManager::GetMainBorrowedSize() const
{
    return mainWatermark.load(std::memory_order_relaxed) - reserveStart;
}

size_t ReserveManager::GetLOSBorrowedSize() const
{
    return reserveEnd - losWatermark.load(std::memory_order_relaxed);
}

} // namespace MapleRuntime
