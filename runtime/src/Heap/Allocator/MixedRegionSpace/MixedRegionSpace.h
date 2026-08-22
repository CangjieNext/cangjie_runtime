// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MIXED_REGION_SPACE_H
#define MRT_MIXED_REGION_SPACE_H

#include "Allocator/RegionSpace.h"
#include "MixedRegionManager.h"

namespace MapleRuntime {

class MixedRegionSpace : public RegionSpace {
public:
    MixedRegionSpace() : RegionSpace(CreateMixedRegionManager()) {};
    ~MixedRegionSpace() override = default;
    static RegionManager* CreateMixedRegionManager()
    {
        RegionManager* regionManager = new (std::nothrow) MixedRegionManager();
        CHECK_DETAIL(regionManager != nullptr, "CANGJIE RUNTIME create region manager failed");
        return regionManager;
    }
    static MixedRegionManager& CastDown(RegionManager& manager)
    {
        return static_cast<MixedRegionManager&>(manager);
    }
    static MixedRegionSpace& CastDown(RegionSpace& regionSpace)
    {
        return static_cast<MixedRegionSpace&>(regionSpace);
    }
    static MixedRegionSpace& CastDown(Allocator& regionSpace)
    {
        return static_cast<MixedRegionSpace&>(regionSpace);
    }

    void AssembleYoungCandidates()
    {
        CastDown(GetRegionManager()).AssembleYoungRegions();
        GetRegionManager().AssemblePinnedGarbageCandidates(false);
        GetRegionManager().AssembleLargeGarbageCandidates();
    }
    void AssembleMixedCandidates()
    {
        CastDown(GetRegionManager()).AssembleMixedRegions();
        GetRegionManager().AssemblePinnedGarbageCandidates(false);
        GetRegionManager().AssembleLargeGarbageCandidates();
    }
    size_t ClassifyCandidateRegions()
    {
        return CastDown(GetRegionManager()).ClassifyYoungRegions();
    }
    void ForwardFromSpace(GCThreadPool* threadPool) override
    {
        CastDown(GetRegionManager()).ForwardFromSpace(threadPool);
    }
};

} // namespace MapleRuntime

#endif // MRT_MIXED_REGION_SPACE_H
