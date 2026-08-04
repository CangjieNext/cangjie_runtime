// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GENERATION_ALLOC_BUFFER_H
#define MRT_GENERATION_ALLOC_BUFFER_H

#include "Allocator/AllocBuffer.h"
#include "Allocator/MixedRegionSpace/MixedRegionManager.h"

namespace MapleRuntime {

class GenerationAllocBuffer : public AllocBuffer {
public:
    GenerationAllocBuffer() = default;
    ~GenerationAllocBuffer() = default;

    static GenerationAllocBuffer* GetGenerationAllocBuffer();
    MAddress AllocateImpl(size_t totalSize);
    MAddress AllocateYoungOnNewRegion(MixedRegionManager &manager, size_t totalSize);
    MAddress AllocateYoungOnSlot(MixedRegionManager& manager, size_t totalSize);
    MAddress AllocateYoung(size_t totalSize);
    MAddress AllocateOld(size_t totalSize);
    MAddress AllocateOldOnSlot(MixedRegionManager& manager, size_t totalSize);
    MAddress AllocateOldOnNewRegion(MixedRegionManager& manager, size_t totalSize);

    RegionInfo* TakeOldAllocRegion()
    {
        RegionInfo* r = oldAllocRegion;
        oldAllocRegion = RegionInfo::NullRegion();
        return r;
    }
private:
    RegionInfo* oldAllocRegion = RegionInfo::NullRegion();
};

} // namespace MapleRuntime

#endif // MRT_GENERATION_ALLOC_BUFFER_H
