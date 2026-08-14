// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_BITMAP_ARENA_H
#define MRT_MS_BITMAP_ARENA_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

enum class MSBitmapPlane : uint8_t {
    ALLOC = 0,
    MARK = 1,
    SCAN = 2,
};

class MSBitmapArena {
public:
    static constexpr size_t kMaxBitmapWords = 64;
    static constexpr size_t kPlaneStrideBytes = kMaxBitmapWords * sizeof(uint64_t);
    static constexpr size_t kRegionsPerChunk = 256;
    static constexpr size_t kChunkBytes = kRegionsPerChunk * kPlaneStrideBytes;

    static void Init(size_t maxRegionCount);
    static void Cleanup();

    static void EnsurePlaneRangeCommitted(
        MSBitmapPlane plane, size_t startRegionIdx, size_t regionCount);
    static void ShrinkToCommittedRegionCount(size_t committedRegionCount);

    static uint64_t* GetAllocPlane(size_t regionIdx);
    static uint64_t* GetMarkPlane(size_t regionIdx);

private:
    static size_t GetChunkCountForRegions(size_t regionCount);
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_BITMAP_ARENA_H
