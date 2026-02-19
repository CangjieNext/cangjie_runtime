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
 * Borrowing strategy (A+B hybrid chunk):
 *   On each borrow call, we first try the internal buffer zone.
 *   If the buffer has insufficient space, we borrow a chunk from RESERVE:
 *     chunkSize = align_up(max(requestSize, max(minChunk, available * 10%)), alignment)
 *   The excess is kept in the buffer for subsequent calls.
 *
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
     * Get available space in RESERVE zone (uncommitted to either side)
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
     * Borrow space for MAIN (grows from low address upward).
     * Internally buffers a chunk; caller just requests exact size needed.
     * @param size Size in bytes to borrow (must be unit-aligned)
     * @return Start address of borrowed region, or 0 if failed
     */
    uintptr_t BorrowForMain(size_t size);

    /**
     * Borrow space for LOS (grows from high address downward).
     * Internally buffers a chunk; caller just requests exact size needed.
     * @param size Size in bytes to borrow (must be unit-aligned)
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
    size_t GetMainChunkBorrowCount() const { return mainChunkBorrowCount; }
    size_t GetLOSChunkBorrowCount() const { return losChunkBorrowCount; }
    size_t GetMainBorrowBytes() const { return mainBorrowBytes; }
    size_t GetLOSBorrowBytes() const { return losBorrowBytes; }

    // Borrow strategy constants
    // Minimum chunk: 16 units (e.g., 256KB on 16KB-page, 64KB on 4KB-page systems)
    static constexpr size_t MIN_BORROW_CHUNK_UNITS = 16;
    // Chunk alignment: same as minimum chunk
    static constexpr size_t BORROW_CHUNK_ALIGN_UNITS = 16;
    // Proportional ratio divisor: borrow available/10 = 10% of remaining RESERVE
    static constexpr size_t BORROW_RATIO_DIVISOR = 10;

private:
    /**
     * Compute chunk size using A+B hybrid strategy.
     * @param requestSize  The actual allocation size in bytes
     * @return Chunk size in bytes to commit from RESERVE watermark
     */
    size_t ComputeBorrowChunk(size_t requestSize) const;

    /**
     * Commit a chunk from RESERVE watermark for MAIN (low→high).
     * Caller must hold borrowMutex.
     * @return Start address of committed chunk, or 0 if failed
     */
    uintptr_t CommitChunkForMain(size_t chunkSize);

    /**
     * Commit a chunk from RESERVE watermark for LOS (high→low).
     * Caller must hold borrowMutex.
     * @return Start address of committed chunk, or 0 if failed
     */
    uintptr_t CommitChunkForLOS(size_t chunkSize);

    // RESERVE zone boundaries
    uintptr_t reserveStart = 0;
    uintptr_t reserveEnd = 0;

    // Unit size (page size)
    size_t unitSize = 0;

    // Watermarks: committed boundaries (only move outward, protected by borrowMutex)
    // MAIN commits from reserveStart upward
    std::atomic<uintptr_t> mainWatermark{0};
    // LOS commits from reserveEnd downward
    std::atomic<uintptr_t> losWatermark{0};

    // Mutex for watermark updates and buffer allocation
    std::mutex borrowMutex;

    // --- MAIN buffer zone (grows low→high) ---
    //   [mainBufCursor ... mainBufCeiling)  = available buffer
    uintptr_t mainBufCursor = 0;
    uintptr_t mainBufCeiling = 0;

    // --- LOS buffer zone (grows high→low) ---
    //   [losBufFloor ... losBufCursor)  = available buffer
    uintptr_t losBufFloor = 0;
    uintptr_t losBufCursor = 0;

    // Statistics
    size_t mainChunkBorrowCount = 0;   // actual watermark commits for MAIN
    size_t losChunkBorrowCount = 0;    // actual watermark commits for LOS
    size_t mainBorrowBytes = 0;        // total bytes committed for MAIN
    size_t losBorrowBytes = 0;         // total bytes committed for LOS
};

} // namespace MapleRuntime

#endif // MRT_RESERVE_MANAGER_H
