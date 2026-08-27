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
        promoteRegionList("promote region list"), ghostPromoteRegionList("ghost promote region list") {}
    ~MixedRegionManager() = default;
    void AssembleYoungRegions()
    {
        size_t unitCount = recentFullRegionList.GetUnitCount();
        size_t youngRegionSize = Heap::GetHeap().GetMaxCapacity() / 3;
        size_t sweptAllocatedSize = sweptYoungRegionList.GetAllocatedSize();
        bool shouldSplit = ShouldSplit(unitCount) && sweptAllocatedSize <= youngRegionSize / 3;
        if (!shouldSplit) {
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
    bool PromoteRegion(RegionInfo* fromRegion);
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
    void RecycleYoungSlotRegion(RegionInfo* region)
    {
        recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
    }
    void RecycleOldSlotRegion(RegionInfo* region)
    {
        sweepableOldRegionList.PrependRegion(region, RegionInfo::RegionType::SWEEP_REGION);
    }
    bool IsYoungSpaceHeavy() const;
    BaseObject* PromoteObject(BaseObject* fromObj, RegionInfo* ghostFromRegion);
    void DumpRegionStats(const char* msg, bool dumpToError = false) const override;
    void PrepareYoungFromRegionList()
    {
        ghostFromRegionList.VisitAllGhostRegions([](RegionInfo* region) {
            DLOG(REGION, "dispel ghost from region %p@[%#zx, %#zx)", region, region->GetRegionStart(),
                 region->GetRegionEnd());
            region->DispelGhostFromRegion();
        });

        fromRegionList.VisitAllRegions([](RegionInfo* region) {
            DLOG(REGION, "prepare from region %p@[%#zx+%zu, %#zx)", region, region->GetRegionStart(),
                 region->GetLiveByteCount(), region->GetRegionEnd());
            region->PrepareForwardableRegion();
        });

        fromRegionList.CopyListTo(ghostFromRegionList);

        ghostPromoteRegionList.VisitAllGhostRegions([](RegionInfo* region) {
            DLOG(REGION, "dispel ghost promote region %p@[%#zx, %#zx)", region, region->GetRegionStart(),
                 region->GetRegionEnd());
            region->DispelGhostFromRegion();
        });
        promoteRegionList.VisitAllRegions([](RegionInfo* region) {
            DLOG(REGION, "prepare promote region %p@[%#zx+%zu, %#zx)", region, region->GetRegionStart(),
                 region->GetLiveByteCount(), region->GetRegionEnd());
            region->PrepareForwardableRegion();
        });
        promoteRegionList.CopyListTo(ghostPromoteRegionList);
    }
    void PrepareMixedFromRegionList()
    {
        RegionManager::PrepareFromRegionList(true);
        ghostPromoteRegionList.VisitAllGhostRegions([](RegionInfo* region) {
            DLOG(REGION, "dispel ghost promote region %p@[%#zx, %#zx)", region, region->GetRegionStart(),
                 region->GetRegionEnd());
            region->DispelGhostFromRegion();
        });
        promoteRegionList.VisitAllRegions([](RegionInfo* region) {
            DLOG(REGION, "prepare promote region %p@[%#zx+%zu, %#zx)", region, region->GetRegionStart(),
                 region->GetLiveByteCount(), region->GetRegionEnd());
            region->PrepareForwardableRegion();
        });
        promoteRegionList.CopyListTo(ghostPromoteRegionList);
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
    void CheckGC() override
    {
        if (IsGcThread()) {
            return;
        }
        Heap& heap = Heap::GetHeap();
        if (!heap.IsGcStarted()) {
            size_t maxCapacity = heap.GetMaxCapacity();
            size_t youngGenSize = (maxCapacity / 3) < 256ULL * 1024 * 1024 ? (maxCapacity / 3) : 256ULL * 1024 * 1024;
            size_t youngAllocated = GetRecentAllocatedSize() + sweptYoungRegionList.GetAllocatedSize() -
                GetFreeSlotTotalSize(sweptYoungRegionList);
            size_t totalAllocated = GetAllocatedSize();
            if (youngAllocated >= youngGenSize * 2 / 3) {
                heap.GetCollector().RequestGC(GC_REASON_HEU, true);
            } else if (totalAllocated >= maxCapacity / 2) {
                heap.GetCollector().RequestGC(GC_REASON_FORCE, true);
            }
        }
    }
    size_t GetAllocatedSize() const override
    {
        size_t total = RegionManager::GetAllocatedSize();
        total += sweepableYoungRegionList.GetAllocatedSize();
        total += sweepableOldRegionList.GetAllocatedSize();
        total += sweptYoungRegionList.GetAllocatedSize();
        total -= GetFreeSlotTotalSize(sweptYoungRegionList);
        total += oldSweptRegionList.GetAllocatedSize();
        total -= GetFreeSlotTotalSize(oldSweptRegionList);
        total += oldRegionList.GetAllocatedSize();
        total += oldFromRegionList.GetAllocatedSize();
        total += promoteRegionList.GetAllocatedSize();
        return total;
    }
    size_t GetUsedUnitCount() const override
    {
        size_t total = RegionManager::GetUsedUnitCount();
        total += sweepableYoungRegionList.GetUnitCount();
        total += sweepableOldRegionList.GetUnitCount();
        total += sweptYoungRegionList.GetUnitCount();
        total += oldSweptRegionList.GetUnitCount();
        total += oldRegionList.GetUnitCount();
        total += oldFromRegionList.GetUnitCount();
        total += promoteRegionList.GetUnitCount();
        return total;
    }
    void AddRawPointerObject(BaseObject* obj) override
    {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        region->IncRawPointerObjectCount();
        if (region->IsFromRegion()) {
            if (region->IsOldRegion()) {
                if (oldFromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                                      RegionInfo::RegionType::SWEEP_REGION)) {
                    GCPhase phase = Heap::GetHeap().GetGCPhase();
                    CHECK(phase != GCPhase::GC_PHASE_FORWARD && phase != GCPhase::GC_PHASE_PREFORWARD &&
                          phase != GCPhase::GC_PHASE_GENERATION_FORWARD &&
                          phase != GCPhase::GC_PHASE_GENERATION_PREFORWARD);
                    if (phase == GCPhase::GC_PHASE_POST_TRACE || phase == GCPhase::GC_PHASE_GENERATION_POST_TRACE) {
                        region->ClearGhostRegionBit();
                        DLOG(TRACE, "dynamicly pin old region %p [%p, %p) and clear ghost bit at phase %u",
                            region, region->GetRegionStart(), region->GetRegionEnd(), phase);
                    } else {
                        DLOG(TRACE, "dynamicly pin old region %p [%p, %p) and keep ghost bit at phase %u",
                            region, region->GetRegionStart(), region->GetRegionEnd(), phase);
                    }
                    sweepableOldRegionList.PrependRegion(region, RegionInfo::RegionType::SWEEP_REGION);
                }
            } else {
                if (fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                                   RegionInfo::RegionType::SWEEP_REGION)) {
                    GCPhase phase = Heap::GetHeap().GetGCPhase();
                    CHECK(phase != GCPhase::GC_PHASE_FORWARD && phase != GCPhase::GC_PHASE_PREFORWARD &&
                          phase != GCPhase::GC_PHASE_GENERATION_FORWARD &&
                          phase != GCPhase::GC_PHASE_GENERATION_PREFORWARD);
                    if (phase == GCPhase::GC_PHASE_POST_TRACE || phase == GCPhase::GC_PHASE_GENERATION_POST_TRACE) {
                        region->ClearGhostRegionBit();
                        DLOG(TRACE, "dynamicly pin young region %p [%p, %p) and clear ghost bit at phase %u",
                            region, region->GetRegionStart(), region->GetRegionEnd(), phase);
                    } else {
                        DLOG(TRACE, "dynamicly pin young region %p [%p, %p) and keep ghost bit at phase %u",
                            region, region->GetRegionStart(), region->GetRegionEnd(), phase);
                    }
                    sweepableYoungRegionList.PrependRegion(region, RegionInfo::RegionType::SWEEP_REGION);
                }
            }
        } else {
            CHECK(region->GetRegionType() != RegionInfo::RegionType::LONE_FROM_REGION);
        }
        // Region may be moved from from region list to promote region list, so it must be double checked after from region
        if (region->IsFromPromoteRegion() && promoteRegionList.TryDeleteRegion(region,
            RegionInfo::RegionType::FROM_PROMOTE_REGION, RegionInfo::RegionType::SWEEP_REGION)) {
            GCPhase phase = Heap::GetHeap().GetGCPhase();
            CHECK(phase != GCPhase::GC_PHASE_FORWARD && phase != GCPhase::GC_PHASE_PREFORWARD &&
                phase != GCPhase::GC_PHASE_GENERATION_FORWARD &&
                phase != GCPhase::GC_PHASE_GENERATION_PREFORWARD);
            if (phase == GCPhase::GC_PHASE_POST_TRACE || phase == GCPhase::GC_PHASE_GENERATION_POST_TRACE) {
                region->ClearGhostRegionBit();
                DLOG(TRACE, "dynamicly pin promoted region %p [%p, %p) and clear ghost bit at phase %u",
                    region, region->GetRegionStart(), region->GetRegionEnd(), phase);
            } else {
                DLOG(TRACE, "dynamicly pin promoted region %p [%p, %p) and keep ghost bit at phase %u",
                    region, region->GetRegionStart(), region->GetRegionEnd(), phase);
            }
            sweepableYoungRegionList.PrependRegion(region, RegionInfo::RegionType::SWEEP_REGION);
        }
    }
    
private:
    static size_t GetFreeSlotTotalSize(const RegionList& list)
    {
        size_t total = 0;
        for (RegionInfo* region = list.GetHeadRegion(); region != nullptr; region = region->GetNextRegion()) {
            RegionFreeSlotList* fsl = region->GetFreeSlotList();
            if (fsl != nullptr) {
                total += fsl->GetTotalSlotSize();
            }
        }
        return total;
    }
    static constexpr uint8_t PROMOTE_AGE = 15;
    void PromoteRegionImpl(RegionInfo* fromRegion);
    // TODO: parameters tuning.
    bool ShouldSplit(size_t unitCount) const
    {
        return unitCount > 4000;
    }
    // TODO: parameters tuning.
    size_t GetMovableThreshold() const
    {
        return 4000;
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
    RegionList promoteRegionList;
    RegionList ghostPromoteRegionList;
};

} // namespace MapleRuntime

#endif // MRT_MIXED_REGION_MANAGER_H
