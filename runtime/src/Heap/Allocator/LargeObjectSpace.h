// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_LARGE_OBJECT_SPACE_H
#define MRT_LARGE_OBJECT_SPACE_H

#ifdef MRT_USE_MARK_SWEEP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

#include "Base/Globals.h"

namespace MapleRuntime {

// ============================================================================
// Tier 1 Large Region List: tracks large objects (16KB < size <= 4MB)
// ============================================================================
// These objects live inside the main heap, allocated via FreeRegionManager.
// Each object occupies one Region group (size rounded up to 32KB multiples).
// Managed with Eager Sweep — swept immediately after marking.
//
// Uses MSRegionInfo::nextRegionIdx/prevRegionIdx for intrusive linking.
//
struct MSRegionInfo;

class LargeRegionList {
public:
    LargeRegionList() = default;

    void Add(MSRegionInfo* region);
    void Remove(MSRegionInfo* region);

    // Eager Sweep: iterate all large Regions, reclaim unmarked ones.
    // Called after MarkTerminate (STW).
    // Returns: number of bytes reclaimed.
    size_t SweepAndReclaim();

    // Visit all large Regions (for GC marking).
    template<typename Visitor>
    void ForEach(Visitor&& visitor);

    size_t Count() const { return count_; }

private:
    std::mutex mu_;
    int32_t headIdx_ = -1;    // INVALID_IDX sentinel
    size_t count_ = 0;
};

// ============================================================================
// Tier 2 LargeObjectSpace: independent mmap for huge objects (>4MB)
// ============================================================================
// Each object gets its own mmap region, munmap on free for immediate OS return.
// Tracked via an intrusive doubly-linked list + pointer set for O(1) Contains.
//
struct LargeObjectInfo {
    uintptr_t baseAddr;          // mmap base address (page-aligned)
    size_t    mappedSize;        // mmap region size (page-aligned)
    size_t    objectSize;        // actual object size
    uint8_t   gcMark;            // GC mark bit (0=dead, 1=alive)
    bool      isTraceAllocated;  // allocated during GC trace phase
    LargeObjectInfo* next;
    LargeObjectInfo* prev;
};

class LargeObjectSpace {
public:
    LargeObjectSpace() = default;
    ~LargeObjectSpace();

    // Allocate a huge object via mmap. Returns object address (after header).
    uintptr_t Allocate(size_t size);

    // Free a huge object: munmap + deregister.
    void Free(uintptr_t addr);

    // O(1) check if addr is a known LOS object.
    bool Contains(uintptr_t addr) const;

    // GC integration
    // Returns true if addr belongs to a LOS object (and marks it), false otherwise.
    bool MarkObject(uintptr_t addr);
    void SweepUnmarkedObjects();
    // Sweep and return stats: count of freed objects and total freed bytes
    void SweepUnmarkedObjectsWithStats(size_t& freedCount, size_t& freedBytes);
    void ClearAllMarks();

    // Visit all objects (for GC root/mark enumeration).
    void ForEachObject(const std::function<void(uintptr_t, size_t)>& visitor);

    size_t GetTotalMappedSize() const { return totalMappedSize_.load(std::memory_order_relaxed); }
    size_t GetObjectCount() const { return objectCount_.load(std::memory_order_relaxed); }

private:
    LargeObjectInfo* FindInfo(uintptr_t addr) const;

    mutable std::mutex mu_;
    LargeObjectInfo* head_ = nullptr;
    std::atomic<size_t> totalMappedSize_{0};
    std::atomic<size_t> objectCount_{0};
};

// ===== Template implementations =====

template<typename Visitor>
void LargeRegionList::ForEach(Visitor&& visitor)
{
    // Implementation deferred to .cpp because it requires MSRegionInfo definition.
    // This placeholder uses the index-based iteration internally.
    (void)visitor;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_LARGE_OBJECT_SPACE_H
