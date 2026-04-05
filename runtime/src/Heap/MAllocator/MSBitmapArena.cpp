// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSBitmapArena.h"

#include <algorithm>
#include <mutex>

#include "Allocator/MemMap.h"
#include "Base/Log.h"

#ifdef _WIN64
#include <memoryapi.h>
#else
#include <sys/mman.h>
#endif

namespace MapleRuntime {
namespace {

std::mutex g_bitmapArenaLock;
MemMap* g_bitmapArenaMap = nullptr;
uintptr_t g_bitmapArenaBase = 0;
size_t g_bitmapArenaMaxRegionCount = 0;
size_t g_bitmapArenaChunkCount = 0;
size_t g_bitmapArenaCommittedChunkCount = 0;

inline uintptr_t GetChunkBase(size_t chunkIdx)
{
    return g_bitmapArenaBase + chunkIdx * MSBitmapArena::kChunkBytes;
}

bool RemapChunkRange(size_t startChunkIdx, size_t chunkCount, bool active)
{
    if (chunkCount == 0) {
        return true;
    }

    uintptr_t chunkBase = GetChunkBase(startChunkIdx);
    size_t chunkBytes = chunkCount * MSBitmapArena::kChunkBytes;

#ifdef _WIN64
    if (active) {
        void* mem = VirtualAlloc(reinterpret_cast<void*>(chunkBase), chunkBytes, MEM_COMMIT, PAGE_READWRITE);
        if (mem == nullptr) {
            LOG(RTLOG_ERROR,
                "[MS-GC] MSBitmapArena commit failed: chunkStart=%zu, chunkCount=%zu, bytes=%zu",
                startChunkIdx, chunkCount, chunkBytes);
            return false;
        }
        return true;
    }

    if (!VirtualFree(reinterpret_cast<void*>(chunkBase), chunkBytes, MEM_DECOMMIT)) {
        LOG(RTLOG_ERROR,
            "[MS-GC] MSBitmapArena decommit failed: chunkStart=%zu, chunkCount=%zu, bytes=%zu",
            startChunkIdx, chunkCount, chunkBytes);
        return false;
    }
    return true;
#else
    int prot = active ? (PROT_READ | PROT_WRITE) : PROT_NONE;
    void* mem = mmap(reinterpret_cast<void*>(chunkBase), chunkBytes, prot,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mem == MAP_FAILED) {
        LOG(RTLOG_ERROR,
            "[MS-GC] MSBitmapArena remap failed: active=%d, chunkStart=%zu, chunkCount=%zu, bytes=%zu",
            active ? 1 : 0, startChunkIdx, chunkCount, chunkBytes);
        return false;
    }
    return true;
#endif
}

} // namespace

size_t MSBitmapArena::GetChunkCountForRegions(size_t regionCount)
{
    if (regionCount == 0) {
        return 0;
    }
    return (regionCount + kRegionsPerChunk - 1) / kRegionsPerChunk;
}

void MSBitmapArena::Init(size_t maxRegionCount, size_t committedRegionCount)
{
    std::lock_guard<std::mutex> lg(g_bitmapArenaLock);

    CHECK_DETAIL(g_bitmapArenaMap == nullptr, "[MS-GC] MSBitmapArena double init");

    size_t totalBytes = maxRegionCount * kBitmapSlotBytes;
    MemMap::Option opt = MemMap::DEFAULT_OPTIONS;
    opt.tag = "ms_bitmap_arena";
#ifndef _WIN64
    opt.prot = PROT_NONE;
    opt.protAll = true;
#endif

    g_bitmapArenaMap = MemMap::MapMemory(totalBytes, totalBytes, opt);
    CHECK_DETAIL(g_bitmapArenaMap != nullptr,
                 "[MS-GC] failed to reserve MS bitmap arena, maxRegionCount=%zu", maxRegionCount);

    g_bitmapArenaBase = reinterpret_cast<uintptr_t>(g_bitmapArenaMap->GetBaseAddr());
    g_bitmapArenaMaxRegionCount = maxRegionCount;
    g_bitmapArenaChunkCount = GetChunkCountForRegions(maxRegionCount);
    g_bitmapArenaCommittedChunkCount = 0;

    size_t initialChunkCount = GetChunkCountForRegions(committedRegionCount);
    if (initialChunkCount != 0) {
        CHECK_DETAIL(RemapChunkRange(0, initialChunkCount, true),
                     "[MS-GC] failed to activate initial bitmap chunks: committedRegions=%zu",
                     committedRegionCount);
        g_bitmapArenaCommittedChunkCount = initialChunkCount;
    }

    LOG(RTLOG_REPORT,
        "[MS-GC] MSBitmapArena initialized: maxRegions=%zu, chunkRegions=%zu, chunkBytes=%zu, initialChunks=%zu",
        maxRegionCount, kRegionsPerChunk, kChunkBytes, initialChunkCount);
}

void MSBitmapArena::Cleanup()
{
    std::lock_guard<std::mutex> lg(g_bitmapArenaLock);

    if (g_bitmapArenaMap == nullptr) {
        return;
    }

    MemMap::DestroyMemMap(g_bitmapArenaMap);
    g_bitmapArenaBase = 0;
    g_bitmapArenaMaxRegionCount = 0;
    g_bitmapArenaChunkCount = 0;
    g_bitmapArenaCommittedChunkCount = 0;
}

void MSBitmapArena::EnsureRegionRangeCommitted(size_t startRegionIdx, size_t regionCount)
{
    if (regionCount == 0) {
        return;
    }

    std::lock_guard<std::mutex> lg(g_bitmapArenaLock);

    CHECK_DETAIL(g_bitmapArenaMap != nullptr, "[MS-GC] MSBitmapArena not initialized");

    size_t requiredRegionCount = startRegionIdx + regionCount;
    CHECK_DETAIL(requiredRegionCount <= g_bitmapArenaMaxRegionCount,
                 "[MS-GC] bitmap arena range overflow: start=%zu, count=%zu, max=%zu",
                 startRegionIdx, regionCount, g_bitmapArenaMaxRegionCount);

    size_t requiredChunkCount = GetChunkCountForRegions(requiredRegionCount);
    CHECK_DETAIL(requiredChunkCount <= g_bitmapArenaChunkCount,
                 "[MS-GC] bitmap arena chunk overflow: required=%zu, max=%zu",
                 requiredChunkCount, g_bitmapArenaChunkCount);
    if (requiredChunkCount <= g_bitmapArenaCommittedChunkCount) {
        return;
    }

    size_t activateChunkStart = g_bitmapArenaCommittedChunkCount;
    size_t activateChunkCount = requiredChunkCount - g_bitmapArenaCommittedChunkCount;
    CHECK_DETAIL(RemapChunkRange(activateChunkStart, activateChunkCount, true),
                 "[MS-GC] failed to activate bitmap chunk range: start=%zu, count=%zu",
                 activateChunkStart, activateChunkCount);
    g_bitmapArenaCommittedChunkCount = requiredChunkCount;
}

void MSBitmapArena::ShrinkToCommittedRegionCount(size_t committedRegionCount)
{
    std::lock_guard<std::mutex> lg(g_bitmapArenaLock);

    if (g_bitmapArenaMap == nullptr) {
        return;
    }

    size_t requiredChunkCount = GetChunkCountForRegions(committedRegionCount);
    CHECK_DETAIL(requiredChunkCount <= g_bitmapArenaChunkCount,
                 "[MS-GC] bitmap arena shrink overflow: required=%zu, max=%zu",
                 requiredChunkCount, g_bitmapArenaChunkCount);
    if (requiredChunkCount >= g_bitmapArenaCommittedChunkCount) {
        return;
    }

    size_t deactivateChunkStart = requiredChunkCount;
    size_t deactivateChunkCount = g_bitmapArenaCommittedChunkCount - requiredChunkCount;
    CHECK_DETAIL(RemapChunkRange(deactivateChunkStart, deactivateChunkCount, false),
                 "[MS-GC] failed to deactivate bitmap chunk range: start=%zu, count=%zu",
                 deactivateChunkStart, deactivateChunkCount);
    g_bitmapArenaCommittedChunkCount = requiredChunkCount;
}

uint64_t* MSBitmapArena::GetSlotBase(size_t regionIdx)
{
    if (g_bitmapArenaMap == nullptr || regionIdx >= g_bitmapArenaMaxRegionCount) {
        return nullptr;
    }
    return reinterpret_cast<uint64_t*>(g_bitmapArenaBase + regionIdx * kBitmapSlotBytes);
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP