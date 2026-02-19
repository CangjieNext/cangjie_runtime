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

    // Initialize borrow watermarks
    mainBorrow.store(reserveStart, std::memory_order_relaxed);
    losBorrow.store(reserveEnd, std::memory_order_relaxed);

    DLOG(REPORT, "ReserveManager initialized: zone [0x%zx, 0x%zx), capacity %zu bytes",
         reserveStart, reserveEnd, GetCapacity());
}

size_t ReserveManager::GetAvailable() const
{
    uintptr_t mainWatermark = mainBorrow.load(std::memory_order_relaxed);
    uintptr_t losWatermark = losBorrow.load(std::memory_order_relaxed);

    if (losWatermark <= mainWatermark) {
        return 0;
    }
    return losWatermark - mainWatermark;
}

uintptr_t ReserveManager::BorrowForMain(size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    uintptr_t currentMain = mainBorrow.load(std::memory_order_relaxed);
    uintptr_t currentLOS = losBorrow.load(std::memory_order_relaxed);
    uintptr_t newMain = currentMain + size;

    // Check if there's enough space
    if (newMain > currentLOS) {
        DLOG(REGION, "ReserveManager: BorrowForMain failed, requested %zu, available %zu",
             size, currentLOS - currentMain);
        return 0;
    }

    mainBorrow.store(newMain, std::memory_order_relaxed);
    DLOG(REGION, "ReserveManager: BorrowForMain %zu bytes @0x%zx, remaining %zu",
         size, currentMain, currentLOS - newMain);
    return currentMain;
}

uintptr_t ReserveManager::BorrowForLOS(size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    uintptr_t currentMain = mainBorrow.load(std::memory_order_relaxed);
    uintptr_t currentLOS = losBorrow.load(std::memory_order_relaxed);
    uintptr_t newLOS = currentLOS - size;

    // Check if there's enough space
    if (newLOS < currentMain) {
        DLOG(REGION, "ReserveManager: BorrowForLOS failed, requested %zu, available %zu",
             size, currentLOS - currentMain);
        return 0;
    }

    losBorrow.store(newLOS, std::memory_order_relaxed);
    DLOG(REGION, "ReserveManager: BorrowForLOS %zu bytes @0x%zx, remaining %zu",
         size, newLOS, newLOS - currentMain);
    return newLOS;
}

void ReserveManager::ReturnFromMain(uintptr_t addr, size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    uintptr_t currentMain = mainBorrow.load(std::memory_order_relaxed);

    // Simple implementation: only allow returning from the top
    // For M1 skeleton, we just log and update watermark if it matches
    if (addr + size == currentMain) {
        mainBorrow.store(addr, std::memory_order_relaxed);
        DLOG(REGION, "ReserveManager: ReturnFromMain %zu bytes @0x%zx", size, addr);
    } else {
        DLOG(REGION, "ReserveManager: ReturnFromMain skipped (not at top), addr 0x%zx, size %zu, current 0x%zx",
             addr, size, currentMain);
    }
}

void ReserveManager::ReturnFromLOS(uintptr_t addr, size_t size)
{
    std::lock_guard<std::mutex> lock(borrowMutex);

    uintptr_t currentLOS = losBorrow.load(std::memory_order_relaxed);

    // Simple implementation: only allow returning from the bottom
    if (addr == currentLOS) {
        losBorrow.store(addr + size, std::memory_order_relaxed);
        DLOG(REGION, "ReserveManager: ReturnFromLOS %zu bytes @0x%zx", size, addr);
    } else {
        DLOG(REGION, "ReserveManager: ReturnFromLOS skipped (not at bottom), addr 0x%zx, size %zu, current 0x%zx",
             addr, size, currentLOS);
    }
}

size_t ReserveManager::GetMainBorrowedSize() const
{
    return mainBorrow.load(std::memory_order_relaxed) - reserveStart;
}

size_t ReserveManager::GetLOSBorrowedSize() const
{
    return reserveEnd - losBorrow.load(std::memory_order_relaxed);
}

} // namespace MapleRuntime
