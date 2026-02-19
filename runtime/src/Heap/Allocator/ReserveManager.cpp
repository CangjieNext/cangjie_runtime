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

    DLOG(REPORT, "ReserveManager initialized: zone [0x%zx, 0x%zx), capacity %zu bytes",
         reserveStart, reserveEnd, GetCapacity());
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

    uintptr_t currentMain = mainWatermark.load(std::memory_order_relaxed);

    // Simple implementation: only allow returning from the top
    if (addr + size == currentMain) {
        mainWatermark.store(addr, std::memory_order_relaxed);
        // Invalidate buffer if it overlaps the returned region
        if (mainBufCeiling > addr) {
            mainBufCursor = 0;
            mainBufCeiling = 0;
        }
        mainReturnBytes += size;
        lastMainReturnCycle = gcCycleCount;  // M5: cooldown tracking
        DLOG(REGION, "ReserveManager: ReturnFromMain %zu bytes @0x%zx, watermark now 0x%zx",
             size, addr, addr);
        return true;
    } else {
        DLOG(REGION, "ReserveManager: ReturnFromMain skipped (not at top), addr 0x%zx, size %zu, current 0x%zx",
             addr, size, currentMain);
        return false;
    }
}

bool ReserveManager::ReturnFromLOS(uintptr_t addr, size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    uintptr_t currentLOS = losWatermark.load(std::memory_order_relaxed);

    // Simple implementation: only allow returning from the bottom
    if (addr == currentLOS) {
        losWatermark.store(addr + size, std::memory_order_relaxed);
        // Invalidate buffer if it overlaps the returned region
        if (losBufFloor < addr + size) {
            losBufFloor = 0;
            losBufCursor = 0;
        }
        losReturnBytes += size;
        lastLOSReturnCycle = gcCycleCount;  // M5: cooldown tracking
        DLOG(REGION, "ReserveManager: ReturnFromLOS %zu bytes @0x%zx, watermark now 0x%zx",
             size, addr, addr + size);
        return true;
    } else {
        DLOG(REGION, "ReserveManager: ReturnFromLOS skipped (not at bottom), addr 0x%zx, size %zu, current 0x%zx",
             addr, size, currentLOS);
        return false;
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
