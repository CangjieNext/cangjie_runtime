// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MIXED_REGION_MANAGER_H
#define MRT_MIXED_REGION_MANAGER_H
#include "Allocator/RegionList.h"
#include "Allocator/RegionManager.h"
#include "GenerationalGC/RememberSet.h"
namespace MapleRuntime {

class MixedRegionManager : public RegionManager {
public:
    MixedRegionManager() : RegionManager(), sweptYoungRegionList("swept young region list"),
        sweepableYoungRegionList("sweepable young region list"), oldSweptRegionList("old swept region list"),
        sweepableOldRegionList("sweepable old region list"), oldRegionList("old region list"),
        oldFromRegionList("old from region list"), ghostOldFromRegionList("ghost old from region list"),
        promptRegionList("prompt region list"), ghostPromptRegionList("ghost prompt region list") {}
    ~MixedRegionManager() = default;
    void AssembleYoungRegions()
    {
        size_t unitCount = recentFullRegionList.GetUnitCount();
        if (!ShouldSplit(unitCount)) {
            fromRegionList.MergeRegionList(recentFullRegionList, RegionInfo::RegionType::FROM_REGION);
        } else {
            RegionList tmpMovable("movable regions cache");
            RegionList tmpSwept("swept regions cache");
            recentFullRegionList.SplitList(tmpMovable, tmpSwept, GetMovableThreshold());
            fromRegionList.MergeRegionList(tmpMovable, RegionInfo::RegionType::FROM_REGION);
            sweepableYoungRegionList.MergeRegionList(tmpSwept, RegionInfo::RegionType::SWEEP_REGION);
        }
    }
    void AssembleMixedRegions()
    {
        AssembleYoungRegions();
        AssembleOldRegions();

    }
    size_t ClassifyYoungRegions();
    bool PromptRegion(RegionInfo* fromRegion);
    void ForwardFromSpace(GCThreadPool* threadPool) override;
    void EnlistOldRegionList(RegionInfo* region) noexcept
    {
        region->SetOldRegionFlag(1);
        oldRegionList.PrependRegion(region, RegionInfo::RegionType::SWEEP_REGION);
    }
    RegionList& GetSweepableRegionList() { return sweepableYoungRegionList; }
    RegionList& GetSweepableOldRegionList() { return sweepableOldRegionList; }
    RegionList& GetSweptRegionList() { return sweptYoungRegionList; }
    RegionList& GetOldSweptRegionList() { return oldSweptRegionList; }
    bool IsYoungSpaceHeavy() const;
    BaseObject* PromptObject(BaseObject* fromObj, RegionInfo* ghostFromRegion);
    void MigrateGhostRegionRSet();
    void PrepareYoungFromRegionList()
    {
        auto& rememberSet = RememberSet::Instance();
        ghostFromRegionList.VisitAllGhostRegions([&rememberSet](RegionInfo* region) {
            DLOG(REGION, "dispel ghost from region %p@[%#zx, %#zx)", region, region->GetRegionStart(),
                 region->GetRegionEnd());
            rememberSet.MigrateGhostRegionSlots(region);
            region->DispelGhostFromRegion();
        });

        fromRegionList.VisitAllRegions([](RegionInfo* region) {
            DLOG(REGION, "prepare from region %p@[%#zx+%zu, %#zx)", region, region->GetRegionStart(),
                 region->GetLiveByteCount(), region->GetRegionEnd());
            region->PrepareForwardableRegion();
        });

        fromRegionList.CopyListTo(ghostFromRegionList);

        ghostPromptRegionList.VisitAllGhostRegions([&rememberSet](RegionInfo* region) {
            DLOG(REGION, "dispel ghost prompt region %p@[%#zx, %#zx)", region, region->GetRegionStart(),
                 region->GetRegionEnd());
            rememberSet.MigrateGhostRegionSlots(region);
            region->DispelGhostFromRegion();
        });
        promptRegionList.VisitAllRegions([](RegionInfo* region) {
            DLOG(REGION, "prepare promt region %p@[%#zx+%zu, %#zx)", region, region->GetRegionStart(),
                 region->GetLiveByteCount(), region->GetRegionEnd());
            region->PrepareForwardableRegion();
        });
        promptRegionList.CopyListTo(ghostPromptRegionList);
    }
    void PrepareMixedFromRegionList()
    {
        RegionManager::PrepareFromRegionList(true);
        ghostPromptRegionList.VisitAllGhostRegions([](RegionInfo* region) {
            DLOG(REGION, "dispel ghost prompt region %p@[%#zx, %#zx)", region, region->GetRegionStart(),
                 region->GetRegionEnd());
            region->DispelGhostFromRegion();
        });
        promptRegionList.VisitAllRegions([](RegionInfo* region) {
            DLOG(REGION, "prepare promt region %p@[%#zx+%zu, %#zx)", region, region->GetRegionStart(),
                 region->GetLiveByteCount(), region->GetRegionEnd());
            region->PrepareForwardableRegion();
        });
        promptRegionList.CopyListTo(ghostPromptRegionList);
        ghostOldFromRegionList.VisitAllGhostRegions([](RegionInfo* region) {
            DLOG(REGION, "dispel ghost old region %p@[%#zx, %#zx)", region, region->GetRegionStart(),
                 region->GetRegionEnd());
            region->DispelGhostFromRegion();
        });
        oldFromRegionList.VisitAllRegions([](RegionInfo* region) {
            DLOG(REGION, "prepare old region %p@[%#zx+%zu, %#zx)", region, region->GetRegionStart(),
                 region->GetLiveByteCount(), region->GetRegionEnd());
            region->PrepareForwardableRegion();
        });
        oldFromRegionList.CopyListTo(ghostOldFromRegionList);
    }
    void PrepareFromRegionList(bool isFull) override
    {
        if (isFull) {
            PrepareMixedFromRegionList();
        } else {
            PrepareYoungFromRegionList();
        }
    }
private:
    static constexpr uint8_t PROMPT_AGE = 15;
    void PromptRegionImpl(RegionInfo* fromRegion);
    // TODO: parameters tuning.
    bool ShouldSplit(size_t unitCount) const
    {
        return unitCount < 400;
    }
    // TODO: parameters tuning.
    size_t GetMovableThreshold() const
    {
        return 400;
    }
    void AssembleOldRegions()
    {
        sweepableOldRegionList.MergeRegionList(oldRegionList, RegionInfo::RegionType::SWEEP_REGION);
    }
    RegionList sweepableYoungRegionList;
    RegionList sweepableOldRegionList;
    RegionList sweptYoungRegionList;
    RegionList oldSweptRegionList;
    RegionList oldRegionList;
    RegionList oldFromRegionList;
    RegionList ghostOldFromRegionList;
    RegionList promptRegionList;
    RegionList ghostPromptRegionList;
};

} // namespace MapleRuntime

#endif // MRT_MIXED_REGION_MANAGER_H
