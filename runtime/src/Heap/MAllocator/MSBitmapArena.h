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

class MSBitmapArena {
public:
    static constexpr size_t kMaxBitmapWords = 64;
    static constexpr size_t kBitmapSlotBytes = kMaxBitmapWords * sizeof(uint64_t) * 2;
    static constexpr size_t kRegionsPerChunk = 256;
    static constexpr size_t kChunkBytes = kRegionsPerChunk * kBitmapSlotBytes;

    static void Init(size_t maxRegionCount, size_t committedRegionCount);
    static void Cleanup();

    static void EnsureRegionRangeCommitted(size_t startRegionIdx, size_t regionCount);
    static void ShrinkToCommittedRegionCount(size_t committedRegionCount);

    static uint64_t* GetSlotBase(size_t regionIdx);

private:
    static size_t GetChunkCountForRegions(size_t regionCount);
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_BITMAP_ARENA_H
