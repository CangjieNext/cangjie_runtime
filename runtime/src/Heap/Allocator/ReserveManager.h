// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RESERVE_MANAGER_H
#define MRT_RESERVE_MANAGER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace MapleRuntime {

/**
 * ReserveManager - Reserve Zone Manager
 *
 * Manages the RESERVE buffer zone between MAIN and LOS spaces.
 * MAIN and LOS can borrow space from RESERVE when needed.
 *
 * Memory layout:
 *   [MAIN Space][RESERVE Zone][LOS Space]
 *               ^             ^
 *        reserveStart    reserveEnd
 *
 * Borrowing:
 *   - MAIN borrows from low address (reserveStart) upward
 *   - LOS borrows from high address (reserveEnd) downward
 */
class ReserveManager {
public:
    ReserveManager() = default;
    ~ReserveManager() = default;

    ReserveManager(const ReserveManager&) = delete;
    ReserveManager& operator=(const ReserveManager&) = delete;

    /**
     * Initialize RESERVE zone with given boundaries.
     * @param start Start address of RESERVE zone (low address)
     * @param end End address of RESERVE zone (high address)
     * @param unitSize Size of each unit (page size)
     */
    void Initialize(uintptr_t start, uintptr_t end, size_t unitSize);

    /**
     * Get RESERVE zone boundaries
     */
    uintptr_t GetReserveStart() const { return reserveStart; }
    uintptr_t GetReserveEnd() const { return reserveEnd; }

    /**
     * Get available space in RESERVE zone
     */
    size_t GetAvailable() const;

    /**
     * Get total capacity of RESERVE zone
     */
    size_t GetCapacity() const { return reserveEnd - reserveStart; }

    /**
     * Check if address is within RESERVE zone
     */
    bool IsInReserve(uintptr_t addr) const { return addr >= reserveStart && addr < reserveEnd; }

    /**
     * Borrow space for MAIN (grows from low address upward)
     * @param size Size in bytes to borrow
     * @return Start address of borrowed region, or 0 if failed
     */
    uintptr_t BorrowForMain(size_t size);

    /**
     * Borrow space for LOS (grows from high address downward)
     * @param size Size in bytes to borrow
     * @return Start address of borrowed region, or 0 if failed
     */
    uintptr_t BorrowForLOS(size_t size);

    /**
     * Return borrowed space from MAIN
     * @param addr Start address of region to return
     * @param size Size in bytes to return
     */
    void ReturnFromMain(uintptr_t addr, size_t size);

    /**
     * Return borrowed space from LOS
     * @param addr Start address of region to return
     * @param size Size in bytes to return
     */
    void ReturnFromLOS(uintptr_t addr, size_t size);

    /**
     * Statistics
     */
    size_t GetMainBorrowedSize() const;
    size_t GetLOSBorrowedSize() const;

private:
    // RESERVE zone boundaries
    uintptr_t reserveStart = 0;
    uintptr_t reserveEnd = 0;

    // Unit size (page size)
    size_t unitSize = 0;

    // Borrow watermarks
    // MAIN borrows from reserveStart upward
    std::atomic<uintptr_t> mainBorrow{0};
    // LOS borrows from reserveEnd downward
    std::atomic<uintptr_t> losBorrow{0};

    // Mutex for complex borrow/return operations
    std::mutex borrowMutex;
};

} // namespace MapleRuntime

#endif // MRT_RESERVE_MANAGER_H
