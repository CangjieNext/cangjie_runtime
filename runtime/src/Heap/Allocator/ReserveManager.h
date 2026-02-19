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
#include <algorithm>

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
     * Return borrowed space from MAIN.
     * Currently only accepts returns from the top of mainWatermark (LIFO).
     * @param addr Start address of region to return
     * @param size Size in bytes to return
     * @return true if the return was accepted (at watermark top), false otherwise
     */
    bool ReturnFromMain(uintptr_t addr, size_t size);

    /**
     * Return borrowed space from LOS.
     * Currently only accepts returns from the bottom of losWatermark (LIFO).
     * @param addr Start address of region to return
     * @param size Size in bytes to return
     * @return true if the return was accepted (at watermark bottom), false otherwise
     */
    bool ReturnFromLOS(uintptr_t addr, size_t size);

    /**
     * M5: Notify ReserveManager that a GC cycle has completed.
     * Used for cooldown tracking.
     */
    void NotifyGCCycle() { gcCycleCount++; }
    size_t GetGCCycleCount() const { return gcCycleCount; }

    /**
     * M5: Check if borrow/return operations are in cooldown.
     * After a return, borrowing is suppressed for BORROW_COOLDOWN_CYCLES.
     * After a borrow, returning is suppressed for RETURN_COOLDOWN_CYCLES.
     * Note: emergency operations (during allocation failure) bypass cooldown.
     */
    bool IsMainBorrowInCooldown() const {
        return (gcCycleCount - lastMainReturnCycle) < BORROW_COOLDOWN_CYCLES;
    }
    bool IsLOSBorrowInCooldown() const {
        return (gcCycleCount - lastLOSReturnCycle) < BORROW_COOLDOWN_CYCLES;
    }
    bool IsMainReturnInCooldown() const {
        return (gcCycleCount - lastMainBorrowCycle) < RETURN_COOLDOWN_CYCLES;
    }
    bool IsLOSReturnInCooldown() const {
        return (gcCycleCount - lastLOSBorrowCycle) < RETURN_COOLDOWN_CYCLES;
    }

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
    size_t GetCooldownSkipCount() const { return cooldownSkipCount; }

    // Borrow strategy constants
    // Minimum chunk: 16 units (e.g., 256KB on 16KB-page, 64KB on 4KB-page systems)
    static constexpr size_t MIN_BORROW_CHUNK_UNITS = 16;
    // Chunk alignment: same as minimum chunk
    static constexpr size_t BORROW_CHUNK_ALIGN_UNITS = 16;
    // Proportional ratio divisor: borrow available/10 = 10% of remaining RESERVE
    static constexpr size_t BORROW_RATIO_DIVISOR = 10;

    // M5: Cooldown constants (in GC cycles)
    static constexpr size_t BORROW_COOLDOWN_CYCLES = 2;  // after return, wait before borrow
    static constexpr size_t RETURN_COOLDOWN_CYCLES = 2;  // after borrow, wait before return

    // M5: Hysteresis thresholds for proactive RESERVE returns
    // Only return borrowed space if usage ratio < this (e.g., MAIN using < 60% of capacity)
    static constexpr double RETURN_USAGE_THRESHOLD = 0.6;
    // Only borrow if usage ratio > this
    static constexpr double BORROW_USAGE_THRESHOLD = 0.8;

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
    size_t mainReturnBytes = 0;        // total bytes returned from MAIN
    size_t losReturnBytes = 0;         // total bytes returned from LOS
    size_t cooldownSkipCount = 0;      // times cooldown prevented an operation

    // M5: GC cycle counter and cooldown tracking
    size_t gcCycleCount = 0;           // incremented by NotifyGCCycle()
    size_t lastMainBorrowCycle = 0;    // cycle when MAIN last borrowed
    size_t lastLOSBorrowCycle = 0;     // cycle when LOS last borrowed
    size_t lastMainReturnCycle = 0;    // cycle when MAIN last returned
    size_t lastLOSReturnCycle = 0;     // cycle when LOS last returned
};

} // namespace MapleRuntime

#endif // MRT_RESERVE_MANAGER_H
