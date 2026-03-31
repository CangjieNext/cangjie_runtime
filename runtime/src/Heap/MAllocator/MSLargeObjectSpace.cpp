// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSLargeObjectSpace.h"
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Base/Log.h"

#ifndef _WIN64
#include <sys/mman.h>
#else
#include <windows.h>
#endif

namespace MapleRuntime {

// ============================================================================
// Tier 1: LargeRegionList — intrusive list via MSRegionInfo indices
// ============================================================================

void LargeRegionList::Add(MSRegionInfo* region)
{
    std::lock_guard<std::mutex> lg(mu_);
    int32_t idx = static_cast<int32_t>(MSRegionInfoArray::GetRegionIdx(region));
    region->prevRegionIdx = MSRegionInfo::INVALID_IDX;
    region->nextRegionIdx = headIdx_;
    if (headIdx_ != MSRegionInfo::INVALID_IDX) {
        MSRegionInfoArray::GetRegionInfoByIdx(headIdx_)->prevRegionIdx = idx;
    }
    headIdx_ = idx;
    count_++;
}

void LargeRegionList::Remove(MSRegionInfo* region)
{
    // Caller must hold mu_ or be in STW
    int32_t prevIdx = region->prevRegionIdx;
    int32_t nextIdx = region->nextRegionIdx;

    if (prevIdx != MSRegionInfo::INVALID_IDX) {
        MSRegionInfoArray::GetRegionInfoByIdx(prevIdx)->nextRegionIdx = nextIdx;
    } else {
        headIdx_ = nextIdx;
    }
    if (nextIdx != MSRegionInfo::INVALID_IDX) {
        MSRegionInfoArray::GetRegionInfoByIdx(nextIdx)->prevRegionIdx = prevIdx;
    }
    region->nextRegionIdx = MSRegionInfo::INVALID_IDX;
    region->prevRegionIdx = MSRegionInfo::INVALID_IDX;
    count_--;
}

size_t LargeRegionList::SweepAndReclaim()
{
    std::lock_guard<std::mutex> lg(mu_);
    size_t reclaimedBytes = 0;
    int32_t idx = headIdx_;

    while (idx != MSRegionInfo::INVALID_IDX) {
        MSRegionInfo* region = MSRegionInfoArray::GetRegionInfoByIdx(idx);
        int32_t nextIdx = region->nextRegionIdx;

        if (region->isMarked) {
            // Alive: clear mark for next GC cycle
            region->isMarked = 0;
        } else {
            // Dead: remove from list, reclaim
            // For LARGE regions, allocPtr holds the true start address
            // (RegionStart() returns regionEnd - MS_REGION_SIZE which is wrong for multi-region objects)
            size_t objSize = region->regionEnd - region->allocPtr;

            // Reset span MSRegionInfo entries before resetting the base entry.
            // RegisterLargeObjectMS initializes span entries with regionType=LARGE
            // and reserved=baseIdx; we must clear them to avoid stale lookups.
            size_t spanCount = (objSize + MS_REGION_SIZE - 1) / MS_REGION_SIZE;
            size_t baseIdx = MSRegionInfoArray::GetRegionIdx(region);
            for (size_t i = 1; i < spanCount; i++) {
                MSRegionInfo* spanInfo = MSRegionInfoArray::GetRegionInfoByIdx(baseIdx + i);
                spanInfo->Reset();
            }

            Remove(region);
            region->Reset();
            reclaimedBytes += objSize;
            // TODO: Return units to FreeRegionManager in Step 3a integration.
            // size_t regionIdx = MSRegionInfoArray::GetRegionIdx(region);
            // size_t unitCount = objSize / UNIT_SIZE;
            // freeRegionManager->AddGarbageUnits(regionIdx * unitsPerRegion, unitCount);
        }
        idx = nextIdx;
    }
    return reclaimedBytes;
}

// ============================================================================
// Tier 2: LargeObjectSpace — independent mmap management
// ============================================================================

LargeObjectSpace::~LargeObjectSpace()
{
    // Clean up all remaining allocations
    std::lock_guard<std::mutex> lg(mu_);
    LargeObjectInfo* curr = head_;
    while (curr != nullptr) {
        LargeObjectInfo* next = curr->next;
#ifndef _WIN64
        munmap(reinterpret_cast<void*>(curr->baseAddr), curr->mappedSize);
#else
        VirtualFree(reinterpret_cast<void*>(curr->baseAddr), 0, MEM_RELEASE);
#endif
        delete curr;
        curr = next;
    }
    head_ = nullptr;
    totalMappedSize_.store(0);
    objectCount_.store(0);
}

uintptr_t LargeObjectSpace::Allocate(size_t size)
{
    // Page-align the allocation
    size_t mappedSize = (size + MRT_PAGE_SIZE - 1) & ~(MRT_PAGE_SIZE - 1);

#ifndef _WIN64
    void* base = mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return 0;
    }
#else
    void* base = VirtualAlloc(nullptr, mappedSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (base == nullptr) {
        return 0;
    }
#endif

    auto* info = new LargeObjectInfo{
        reinterpret_cast<uintptr_t>(base), mappedSize, size,
        0,      // gcMark
        false,  // isTraceAllocated
        nullptr, nullptr
    };

    {
        std::lock_guard<std::mutex> lg(mu_);
        info->next = head_;
        if (head_ != nullptr) {
            head_->prev = info;
        }
        head_ = info;
    }

    totalMappedSize_.fetch_add(mappedSize, std::memory_order_relaxed);
    objectCount_.fetch_add(1, std::memory_order_relaxed);

    // Return the base address directly (HEADER_SIZE = 0 in current design)
    return reinterpret_cast<uintptr_t>(base);
}

void LargeObjectSpace::Free(uintptr_t addr)
{
    std::lock_guard<std::mutex> lg(mu_);
    LargeObjectInfo* info = FindInfo(addr);
    if (info == nullptr) {
        return;
    }

    // Remove from linked list
    if (info->prev != nullptr) {
        info->prev->next = info->next;
    }
    if (info->next != nullptr) {
        info->next->prev = info->prev;
    }
    if (info == head_) {
        head_ = info->next;
    }

    totalMappedSize_.fetch_sub(info->mappedSize, std::memory_order_relaxed);
    objectCount_.fetch_sub(1, std::memory_order_relaxed);

#ifndef _WIN64
    munmap(reinterpret_cast<void*>(info->baseAddr), info->mappedSize);
#else
    VirtualFree(reinterpret_cast<void*>(info->baseAddr), 0, MEM_RELEASE);
#endif
    delete info;
}

bool LargeObjectSpace::Contains(uintptr_t addr) const
{
    std::lock_guard<std::mutex> lg(mu_);
    return FindInfo(addr) != nullptr;
}

LargeObjectInfo* LargeObjectSpace::FindInfo(uintptr_t addr) const
{
    // Linear scan of the linked list. For a small number of huge objects
    // this is acceptable. If performance becomes an issue, switch to
    // an unordered_map keyed on page-aligned base address.
    for (LargeObjectInfo* curr = head_; curr != nullptr; curr = curr->next) {
        if (addr >= curr->baseAddr && addr < curr->baseAddr + curr->mappedSize) {
            return curr;
        }
    }
    return nullptr;
}

bool LargeObjectSpace::MarkObject(uintptr_t addr)
{
    std::lock_guard<std::mutex> lg(mu_);
    LargeObjectInfo* info = FindInfo(addr);
    if (info != nullptr) {
        info->gcMark = 1;
        return true;
    }
    return false;
}

void LargeObjectSpace::SweepUnmarkedObjects()
{
    size_t freedCount = 0;
    size_t freedBytes = 0;
    SweepUnmarkedObjectsWithStats(freedCount, freedBytes);
}

void LargeObjectSpace::SweepUnmarkedObjectsWithStats(size_t& freedCount, size_t& freedBytes)
{
    freedCount = 0;
    freedBytes = 0;
    std::lock_guard<std::mutex> lg(mu_);
    LargeObjectInfo* curr = head_;
    while (curr != nullptr) {
        LargeObjectInfo* next = curr->next;
        if (curr->gcMark == 0) {
            // Dead: remove and munmap
            if (curr->prev != nullptr) {
                curr->prev->next = curr->next;
            }
            if (curr->next != nullptr) {
                curr->next->prev = curr->prev;
            }
            if (curr == head_) {
                head_ = curr->next;
            }
            freedBytes += curr->mappedSize;
            freedCount++;
            totalMappedSize_.fetch_sub(curr->mappedSize, std::memory_order_relaxed);
            objectCount_.fetch_sub(1, std::memory_order_relaxed);
#ifndef _WIN64
            munmap(reinterpret_cast<void*>(curr->baseAddr), curr->mappedSize);
#else
            VirtualFree(reinterpret_cast<void*>(curr->baseAddr), 0, MEM_RELEASE);
#endif
            delete curr;
        } else {
            // Alive: clear mark for next cycle
            curr->gcMark = 0;
        }
        curr = next;
    }
}

void LargeObjectSpace::ClearAllMarks()
{
    std::lock_guard<std::mutex> lg(mu_);
    for (LargeObjectInfo* curr = head_; curr != nullptr; curr = curr->next) {
        curr->gcMark = 0;
    }
}

void LargeObjectSpace::ForEachObject(const std::function<void(uintptr_t, size_t)>& visitor)
{
    std::lock_guard<std::mutex> lg(mu_);
    for (LargeObjectInfo* curr = head_; curr != nullptr; curr = curr->next) {
        visitor(curr->baseAddr, curr->objectSize);
    }
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
