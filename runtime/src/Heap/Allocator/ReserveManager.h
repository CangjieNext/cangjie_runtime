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
#include <cstring>
#include <mutex>
#include <algorithm>
#include <vector>

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
 * Borrowing strategy (runway extension with A+B hybrid chunk):
 *   On each borrow call, we commit a chunk from the RESERVE watermark to extend
 *   the caller's inactiveZone runway. The caller's normal allocation (fetch_add)
 *   then uses the extended space.
 *     chunkSize = align_up(max(requestSize, max(minChunk, available * 10%)), alignment)
 *
 *   - MAIN borrows from low address (reserveStart) upward → extends mainWatermark
 *   - LOS borrows from high address (reserveEnd) downward → extends losWatermark
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
     * Extend MAIN runway by borrowing from RESERVE (grows upward).
     * Commits a chunk from mainWatermark, extending the effective MAIN boundary.
     * Caller's inactiveZone can then expand into the borrowed space normally.
     * @param size Minimum bytes needed (chunk may be larger for amortization)
     * @return Bytes actually committed (>0 on success, 0 on failure)
     */
    size_t BorrowForMain(size_t size);

    /**
     * Extend LOS runway by borrowing from RESERVE (grows downward).
     * Commits a chunk from losWatermark, extending the effective LOS boundary.
     * Caller's losInactiveZone can then expand into the borrowed space normally.
     * @param size Minimum bytes needed (chunk may be larger for amortization)
     * @return Bytes actually committed (>0 on success, 0 on failure)
     */
    size_t BorrowForLOS(size_t size);

    /**
     * Bulk reclaim MAIN-borrowed RESERVE space after ShrinkInactiveZone Phase B.
     * Directly pushes mainWatermark to newWatermark without bitmap.
     * @param newWatermark New mainWatermark value (must be <= current mainWatermark
     *                     and >= reserveStart). Excess MAIN buffer is invalidated.
     * @return Bytes reclaimed (oldWatermark - newWatermark)
     */
    size_t ReclaimMainBorrowed(uintptr_t newWatermark);

    /**
     * Bulk reclaim LOS-borrowed RESERVE space after CompactLOS.
     * Directly pushes losWatermark to newWatermark without bitmap.
     * @param newWatermark New losWatermark value (must be >= current losWatermark
     *                     and <= reserveEnd). Excess LOS buffer is invalidated.
     * @return Bytes reclaimed (newWatermark - oldWatermark)
     */
    size_t ReclaimLOSBorrowed(uintptr_t newWatermark);

    /**
     * M5: Notify ReserveManager that a GC cycle has completed.
     */
    void NotifyGCCycle() { gcCycleCount++; }
    size_t GetGCCycleCount() const { return gcCycleCount; }

    /**
     * Get MAIN watermark (current high-water of MAIN's borrowed area).
     * Addresses in [reserveStart, mainWatermark) are MAIN-borrowed.
     */
    uintptr_t GetMainWatermark() const { return mainWatermark.load(std::memory_order_relaxed); }
    uintptr_t GetLOSWatermark() const { return losWatermark.load(std::memory_order_relaxed); }

    /**
     * Statistics
     */
    size_t GetMainBorrowedSize() const;
    size_t GetLOSBorrowedSize() const;
    size_t GetMainChunkBorrowCount() const { return mainChunkBorrowCount; }
    size_t GetLOSChunkBorrowCount() const { return losChunkBorrowCount; }
    size_t GetMainBorrowBytes() const { return mainBorrowBytes; }
    size_t GetLOSBorrowBytes() const { return losBorrowBytes; }
    size_t GetMainReturnBytes() const { return mainReturnBytes; }
    size_t GetLOSReturnBytes() const { return losReturnBytes; }

    // Borrow strategy constants
    // Minimum chunk: 16 units (e.g., 256KB on 16KB-page, 64KB on 4KB-page systems)
    static constexpr size_t MIN_BORROW_CHUNK_UNITS = 16;
    // Chunk alignment: same as minimum chunk
    static constexpr size_t BORROW_CHUNK_ALIGN_UNITS = 16;
    // Proportional ratio divisor: borrow available/10 = 10% of remaining RESERVE
    static constexpr size_t BORROW_RATIO_DIVISOR = 10;

    // M5: Cooldown constants (in GC cycles) — reserved for future use
    static constexpr size_t BORROW_COOLDOWN_CYCLES = 2;
    static constexpr size_t RETURN_COOLDOWN_CYCLES = 2;

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
    size_t totalReserveUnits = 0;  // (reserveEnd - reserveStart) / unitSize

    // Watermarks: committed boundaries (only move outward, protected by borrowMutex)
    // MAIN commits from reserveStart upward
    std::atomic<uintptr_t> mainWatermark{0};
    // LOS commits from reserveEnd downward
    std::atomic<uintptr_t> losWatermark{0};

    // Mutex for watermark updates
    std::mutex borrowMutex;

    // Statistics
    size_t mainChunkBorrowCount = 0;   // actual watermark commits for MAIN
    size_t losChunkBorrowCount = 0;    // actual watermark commits for LOS
    size_t mainBorrowBytes = 0;        // total bytes committed for MAIN
    size_t losBorrowBytes = 0;         // total bytes committed for LOS
    size_t mainReturnBytes = 0;        // total bytes returned from MAIN
    size_t losReturnBytes = 0;         // total bytes returned from LOS

    // M5: GC cycle counter
    size_t gcCycleCount = 0;           // incremented by NotifyGCCycle()
};

} // namespace MapleRuntime

#endif // MRT_RESERVE_MANAGER_H
