// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSBitmapArena.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <mutex>

#include "Allocator/MemMap.h"
#include "Base/Log.h"
#include "Heap/MSGCFastPath.h"

#ifdef _WIN64
#include <errhandlingapi.h>
#include <memoryapi.h>
#else
#include <sys/mman.h>
#endif

namespace MapleRuntime {
namespace {

std::mutex g_bitmapArenaLock;

struct MSBitmapPlaneState {
    MemMap* map = nullptr;
    uintptr_t base = 0;
    size_t committedChunkCount = 0;
    const char* tag = nullptr;
};

std::array<MSBitmapPlaneState, 3> g_bitmapPlanes {{
    {nullptr, 0, 0, "ms_alloc_plane"},
    {nullptr, 0, 0, "ms_mark_plane"},
    {nullptr, 0, 0, "ms_scan_plane"},
}};
size_t g_bitmapArenaMaxRegionCount = 0;
size_t g_bitmapArenaChunkCount = 0;

inline size_t PlaneIndex(MSBitmapPlane plane)
{
    return static_cast<size_t>(plane);
}

inline MSBitmapPlaneState& GetPlaneState(MSBitmapPlane plane)
{
    size_t planeIdx = PlaneIndex(plane);
    CHECK_DETAIL(planeIdx < g_bitmapPlanes.size(),
                 "[MS-GC] invalid bitmap plane=%zu", planeIdx);
    return g_bitmapPlanes[planeIdx];
}

inline uintptr_t GetChunkBase(const MSBitmapPlaneState& state, size_t chunkIdx)
{
    return state.base + chunkIdx * MSBitmapArena::kChunkBytes;
}

bool RemapChunkRange(
    MSBitmapPlaneState& state, size_t startChunkIdx, size_t chunkCount, bool active)
{
    if (chunkCount == 0) {
        return true;
    }

    uintptr_t chunkBase = GetChunkBase(state, startChunkIdx);
    size_t chunkBytes = chunkCount * MSBitmapArena::kChunkBytes;

#ifdef _WIN64
    if (active) {
        void* mem = VirtualAlloc(reinterpret_cast<void*>(chunkBase), chunkBytes, MEM_COMMIT, PAGE_READWRITE);
        if (mem == nullptr) {
            LOG(RTLOG_ERROR,
                "[MS-GC] MSBitmapArena commit failed: chunkStart=%zu, chunkCount=%zu, bytes=%zu, error=%lu",
                startChunkIdx, chunkCount, chunkBytes, static_cast<unsigned long>(GetLastError()));
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
            "[MS-GC] MSBitmapArena remap failed: active=%d, chunkStart=%zu, chunkCount=%zu, bytes=%zu, errno=%d",
            active ? 1 : 0, startChunkIdx, chunkCount, chunkBytes, errno);
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

void MSBitmapArena::Init(size_t maxRegionCount)
{
    std::lock_guard<std::mutex> lg(g_bitmapArenaLock);

    for (const MSBitmapPlaneState& state : g_bitmapPlanes) {
        CHECK_DETAIL(state.map == nullptr, "[MS-GC] MSBitmapArena double init");
    }

    CHECK_DETAIL(maxRegionCount <= SIZE_MAX / kPlaneStrideBytes,
                 "[MS-GC] bitmap plane size overflow: maxRegions=%zu", maxRegionCount);
    size_t totalBytes = maxRegionCount * kPlaneStrideBytes;
    g_bitmapArenaMaxRegionCount = maxRegionCount;
    g_bitmapArenaChunkCount = GetChunkCountForRegions(maxRegionCount);

    for (MSBitmapPlaneState& state : g_bitmapPlanes) {
        MemMap::Option opt = MemMap::DEFAULT_OPTIONS;
        opt.tag = state.tag;
#ifndef _WIN64
        opt.prot = PROT_NONE;
        opt.protAll = true;
#endif

        state.map = MemMap::MapMemory(totalBytes, totalBytes, opt);
        CHECK_DETAIL(state.map != nullptr,
                     "[MS-GC] failed to reserve %s, maxRegionCount=%zu",
                     state.tag, maxRegionCount);
        state.base = reinterpret_cast<uintptr_t>(state.map->GetBaseAddr());
        state.committedChunkCount = 0;
    }

    PublishMSGCFastPathPlaneBases(
        g_bitmapPlanes[PlaneIndex(MSBitmapPlane::ALLOC)].base,
        g_bitmapPlanes[PlaneIndex(MSBitmapPlane::MARK)].base,
        g_bitmapPlanes[PlaneIndex(MSBitmapPlane::SCAN)].base);

    LOG(RTLOG_REPORT,
        "[MS-GC] MSBitmapArena initialized: maxRegions=%zu, planeStride=%zu, "
        "chunkRegions=%zu, chunkBytes=%zu",
        maxRegionCount, kPlaneStrideBytes, kRegionsPerChunk, kChunkBytes);
}

void MSBitmapArena::Cleanup()
{
    std::lock_guard<std::mutex> lg(g_bitmapArenaLock);

    if (g_bitmapPlanes[0].map == nullptr) {
        return;
    }

    PublishMSGCFastPathPlaneBases(0, 0, 0);
    for (MSBitmapPlaneState& state : g_bitmapPlanes) {
        if (state.map != nullptr) {
            MemMap::DestroyMemMap(state.map);
        }
        state.map = nullptr;
        state.base = 0;
        state.committedChunkCount = 0;
    }
    g_bitmapArenaMaxRegionCount = 0;
    g_bitmapArenaChunkCount = 0;
}

bool MSBitmapArena::EnsurePlaneRangeCommitted(
    MSBitmapPlane plane, size_t startRegionIdx, size_t regionCount)
{
    if (regionCount == 0) {
        return true;
    }

    std::lock_guard<std::mutex> lg(g_bitmapArenaLock);

    MSBitmapPlaneState& state = GetPlaneState(plane);
    CHECK_DETAIL(state.map != nullptr, "[MS-GC] MSBitmapArena not initialized");

    size_t requiredRegionCount = startRegionIdx + regionCount;
    CHECK_DETAIL(requiredRegionCount <= g_bitmapArenaMaxRegionCount,
                 "[MS-GC] bitmap arena range overflow: start=%zu, count=%zu, max=%zu",
                 startRegionIdx, regionCount, g_bitmapArenaMaxRegionCount);

    size_t requiredChunkCount = GetChunkCountForRegions(requiredRegionCount);
    CHECK_DETAIL(requiredChunkCount <= g_bitmapArenaChunkCount,
                 "[MS-GC] bitmap arena chunk overflow: required=%zu, max=%zu",
                 requiredChunkCount, g_bitmapArenaChunkCount);
    if (requiredChunkCount <= state.committedChunkCount) {
        return true;
    }

    size_t activateChunkStart = state.committedChunkCount;
    size_t activateChunkCount = requiredChunkCount - state.committedChunkCount;
    if (!RemapChunkRange(state, activateChunkStart, activateChunkCount, true)) {
        return false;
    }
    state.committedChunkCount = requiredChunkCount;
    return true;
}

namespace {
uint64_t* GetRegionPlane(MSBitmapPlane plane, size_t regionIdx)
{
    const MSBitmapPlaneState& state = GetPlaneState(plane);
    if (state.map == nullptr || regionIdx >= g_bitmapArenaMaxRegionCount) {
        return nullptr;
    }
    return reinterpret_cast<uint64_t*>(
        state.base + regionIdx * MSBitmapArena::kPlaneStrideBytes);
}
} // namespace

uint64_t* MSBitmapArena::GetAllocPlane(size_t regionIdx)
{
    return GetRegionPlane(MSBitmapPlane::ALLOC, regionIdx);
}

uint64_t* MSBitmapArena::GetMarkPlane(size_t regionIdx)
{
    return GetRegionPlane(MSBitmapPlane::MARK, regionIdx);
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
