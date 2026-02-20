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

    // Buffer zones start empty
    mainBufCursor = 0;
    mainBufCeiling = 0;
    losBufFloor = 0;
    losBufCursor = 0;

    // Initialize hole bitmap (1 bit per RESERVE unit, all zero)
    totalReserveUnits = (reserveEnd - reserveStart) / unitSize;
    returnBitmap.assign((totalReserveUnits + 7) / 8, 0);

    DLOG(REPORT, "ReserveManager initialized: zone [0x%zx, 0x%zx), capacity %zu bytes, "
         "%zu units, bitmap %zu bytes",
         reserveStart, reserveEnd, GetCapacity(), totalReserveUnits, returnBitmap.size());
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

// ===== Public borrow API (with internal buffer zone management) =====

uintptr_t ReserveManager::BorrowForMain(size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    // Try existing buffer first
    if (mainBufCursor + size <= mainBufCeiling) {
        uintptr_t addr = mainBufCursor;
        mainBufCursor += size;
        DLOG(REGION, "ReserveManager: BorrowForMain (buffer hit) %zu bytes @0x%zx, buffer remaining %zu",
             size, addr, mainBufCeiling - mainBufCursor);
        return addr;
    }

    // Buffer insufficient — compute chunk and commit from watermark
    size_t chunkSize = ComputeBorrowChunk(size);
    uintptr_t chunkAddr = CommitChunkForMain(chunkSize);
    if (chunkAddr == 0) {
        return 0;
    }

    // Set up new buffer zone
    mainBufCursor = chunkAddr;
    mainBufCeiling = chunkAddr + chunkSize;
    mainChunkBorrowCount++;
    mainBorrowBytes += chunkSize;
    lastMainBorrowCycle = gcCycleCount;  // M5: cooldown tracking

    // Allocate from new buffer
    uintptr_t addr = mainBufCursor;
    mainBufCursor += size;
    DLOG(REGION, "ReserveManager: BorrowForMain (new chunk [0x%zx, 0x%zx) = %zu) %zu bytes @0x%zx, "
         "buffer remaining %zu",
         chunkAddr, chunkAddr + chunkSize, chunkSize, size, addr, mainBufCeiling - mainBufCursor);
    return addr;
}

uintptr_t ReserveManager::BorrowForLOS(size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    // Try existing buffer first (LOS grows high→low)
    if (losBufCursor >= losBufFloor + size) {
        uintptr_t addr = losBufCursor - size;
        losBufCursor = addr;
        DLOG(REGION, "ReserveManager: BorrowForLOS (buffer hit) %zu bytes @0x%zx, buffer remaining %zu",
             size, addr, losBufCursor - losBufFloor);
        return addr;
    }

    // Buffer insufficient — compute chunk and commit from watermark
    size_t chunkSize = ComputeBorrowChunk(size);
    uintptr_t chunkAddr = CommitChunkForLOS(chunkSize);
    if (chunkAddr == 0) {
        return 0;
    }

    // Set up new buffer zone: chunk is [chunkAddr, chunkAddr + chunkSize)
    losBufFloor = chunkAddr;
    losBufCursor = chunkAddr + chunkSize;
    losChunkBorrowCount++;
    losBorrowBytes += chunkSize;
    lastLOSBorrowCycle = gcCycleCount;  // M5: cooldown tracking

    // Allocate from new buffer (high→low)
    uintptr_t addr = losBufCursor - size;
    losBufCursor = addr;
    DLOG(REGION, "ReserveManager: BorrowForLOS (new chunk [0x%zx, 0x%zx) = %zu) %zu bytes @0x%zx, "
         "buffer remaining %zu",
         chunkAddr, chunkAddr + chunkSize, chunkSize, size, addr, losBufCursor - losBufFloor);
    return addr;
}

bool ReserveManager::ReturnFromMain(uintptr_t addr, size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    // Mark the returned units in the hole bitmap
    size_t startIdx = AddrToUnitIndex(addr);
    size_t count = size / unitSize;
    SetBitmapRange(startIdx, count);

    DLOG(REGION, "ReserveManager: ReturnFromMain marked bitmap [%zu, %zu) (%zu bytes @0x%zx)",
         startIdx, startIdx + count, size, addr);

    // Try to push mainWatermark back by scanning contiguous returned bits
    CoalesceMainWatermark();

    return true;  // always succeeds — bitmap records the return
}

bool ReserveManager::ReturnFromLOS(uintptr_t addr, size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    // Mark the returned units in the hole bitmap
    size_t startIdx = AddrToUnitIndex(addr);
    size_t count = size / unitSize;
    SetBitmapRange(startIdx, count);

    DLOG(REGION, "ReserveManager: ReturnFromLOS marked bitmap [%zu, %zu) (%zu bytes @0x%zx)",
         startIdx, startIdx + count, size, addr);

    // Try to push losWatermark forward by scanning contiguous returned bits
    CoalesceLOSWatermark();

    return true;  // always succeeds — bitmap records the return
}

// ===== Hole bitmap helpers =====

void ReserveManager::SetBitmapRange(size_t startIdx, size_t count)
{
    for (size_t i = startIdx; i < startIdx + count && i < totalReserveUnits; ++i) {
        returnBitmap[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    }
}

void ReserveManager::ClearBitmapRange(size_t startIdx, size_t count)
{
    for (size_t i = startIdx; i < startIdx + count && i < totalReserveUnits; ++i) {
        returnBitmap[i / 8] &= static_cast<uint8_t>(~(1u << (i % 8)));
    }
}

void ReserveManager::CoalesceMainWatermark()
{
    // MAIN borrowed area: [reserveStart, mainWatermark) = units [0, wmIdx)
    // Scan from wmIdx-1 downward: find contiguous returned (bit=1) units.
    uintptr_t currentMain = mainWatermark.load(std::memory_order_relaxed);
    size_t wmIdx = AddrToUnitIndex(currentMain);

    size_t newWmIdx = wmIdx;
    while (newWmIdx > 0 && IsBitSet(newWmIdx - 1)) {
        newWmIdx--;
    }

    if (newWmIdx < wmIdx) {
        // Reclaim contiguous returned units: clear their bits, move watermark
        ClearBitmapRange(newWmIdx, wmIdx - newWmIdx);
        uintptr_t newWatermark = UnitIndexToAddr(newWmIdx);
        mainWatermark.store(newWatermark, std::memory_order_relaxed);

        // Invalidate MAIN buffer if it overlaps the reclaimed region
        if (mainBufCeiling > newWatermark) {
            mainBufCursor = 0;
            mainBufCeiling = 0;
        }

        size_t reclaimedBytes = (wmIdx - newWmIdx) * unitSize;
        mainReturnBytes += reclaimedBytes;
        lastMainReturnCycle = gcCycleCount;

        DLOG(REGION, "ReserveManager: CoalesceMainWatermark reclaimed %zu units [%zu, %zu), "
             "watermark 0x%zx \u2192 0x%zx",
             wmIdx - newWmIdx, newWmIdx, wmIdx, currentMain, newWatermark);
    }
}

void ReserveManager::CoalesceLOSWatermark()
{
    // LOS borrowed area: [losWatermark, reserveEnd) = units [losWmIdx, totalReserveUnits)
    // Scan from losWmIdx upward: find contiguous returned (bit=1) units.
    uintptr_t currentLOS = losWatermark.load(std::memory_order_relaxed);
    size_t losWmIdx = AddrToUnitIndex(currentLOS);

    size_t newLosWmIdx = losWmIdx;
    while (newLosWmIdx < totalReserveUnits && IsBitSet(newLosWmIdx)) {
        newLosWmIdx++;
    }

    if (newLosWmIdx > losWmIdx) {
        // Reclaim contiguous returned units: clear their bits, move watermark
        ClearBitmapRange(losWmIdx, newLosWmIdx - losWmIdx);
        uintptr_t newWatermark = UnitIndexToAddr(newLosWmIdx);
        losWatermark.store(newWatermark, std::memory_order_relaxed);

        // Invalidate LOS buffer if it overlaps the reclaimed region
        if (losBufFloor < newWatermark) {
            losBufFloor = 0;
            losBufCursor = 0;
        }

        size_t reclaimedBytes = (newLosWmIdx - losWmIdx) * unitSize;
        losReturnBytes += reclaimedBytes;
        lastLOSReturnCycle = gcCycleCount;

        DLOG(REGION, "ReserveManager: CoalesceLOSWatermark reclaimed %zu units [%zu, %zu), "
             "watermark 0x%zx \u2192 0x%zx",
             newLosWmIdx - losWmIdx, losWmIdx, newLosWmIdx, currentLOS, newWatermark);
    }
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
