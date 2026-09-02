// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "MixedRegionManager.h"
#include "GenerationalGC/GenerationAllocBuffer.h"
#include "Collector/CopyCollector.h"
#include "Collector/CardTable.h"
#include "Collector/TracingCollector.h"
#include "GenerationalGC/RememberSet.h"
#include "Heap/Heap.h"
#include "HeapWork.h"
#include "GcThreadPool.h"
#include "ObjectModel/RefField.inline.h"
#include <unistd.h>

namespace MapleRuntime {

void MixedRegionManager::DumpRegionStats(const char* msg, bool dumpToError) const
{
    RegionManager::DumpRegionStats(msg, dumpToError);

    auto dumpListStats = [dumpToError](const char* name, const RegionList& list, bool dumpFreeSlotSize) {
        size_t regions = list.GetRegionCount();
        size_t units = list.GetUnitCount();
        size_t size = units * RegionInfo::UNIT_SIZE;
        size_t allocated = list.GetAllocatedSize();
        if (dumpFreeSlotSize) {
            size_t freeSlotSize = GetFreeSlotTotalSize(list);
            const char* format = "\t%s %zu: %zu units (%zu B, alloc %zu, free slot %zu)";
            VLOG(REPORT, format, name, regions, units, size, allocated, freeSlotSize);
            if (dumpToError) {
                LOG(RTLOG_ERROR, format, name, regions, units, size, allocated, freeSlotSize);
            }
            return;
        }
        const char* format = "\t%s %zu: %zu units (%zu B, alloc %zu)";
        VLOG(REPORT, format, name, regions, units, size, allocated);
        if (dumpToError) {
            LOG(RTLOG_ERROR, format, name, regions, units, size, allocated);
        }
    };

    dumpListStats("sweepable young regions", sweepableYoungRegionList, false);
    dumpListStats("sweepable old regions", sweepableOldRegionList, false);
    dumpListStats("swept young regions", sweptYoungRegionList, true);
    dumpListStats("swept old regions", oldSweptRegionList, true);
    dumpListStats("old regions", oldRegionList, false);
    dumpListStats("old from regions", oldFromRegionList, false);
    dumpListStats("ghost old from regions", ghostOldFromRegionList, false);
    dumpListStats("promote regions", promoteRegionList, false);
    dumpListStats("ghost promote regions", ghostPromoteRegionList, false);
}

size_t MixedRegionManager::ClassifyYoungRegions()
{
    size_t forwardBytes = 0;
    size_t floatingGarbage = 0;
    size_t oldFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    double exempt = exemptedRegionThreshold;
    auto visitor = [this, exempt, &floatingGarbage, &forwardBytes](RegionInfo* fromRegion) {
        long rawPtrCnt = fromRegion->GetRawPointerObjectCount();
        if (rawPtrCnt == 0 && fromRegion->IsPromoted()) {
            RemoveRegionLocked(&fromRegionList, fromRegion);
            promoteRegionList.PrependRegion(fromRegion, RegionInfo::RegionType::FROM_PROMOTE_REGION);
            return;
        }
        size_t threshold = static_cast<size_t>(exempt * fromRegion->GetRegionSize());
        size_t liveBytes = fromRegion->GetLiveByteCount();
        if (liveBytes > threshold || rawPtrCnt > 0) { // ignore this region
            RegionInfo* del = fromRegion;
            DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) exempted by forwarding: %zu units, %u live bytes", del,
                del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(),
                del->GetUnitCount(), del->GetLiveByteCount());

            CHECK(del->IsFromRegion());
            RemoveRegionLocked(&fromRegionList, del);
            del->ClearGhostRegionBit();
            sweepableYoungRegionList.PrependRegion(del, RegionInfo::RegionType::SWEEP_REGION);
            floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
        } else {
            forwardBytes += liveBytes;
        }
    };
    fromRegionList.VisitAllRegions(visitor);

    auto promoteSweepVisitor = [this](RegionInfo* region) {
        long rawPtrCnt = region->GetRawPointerObjectCount();
        region->ClearGhostRegionBit();
        // If the region route kind is sweepable region, it may lead to promotion infomation deleted
        // when the region is rebuilt as free region.
        region->ClearFreeSlotList();
        if (rawPtrCnt == 0 && region->IsPromoted()) {
            RemoveRegionLocked(&sweepableYoungRegionList, region);
            recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
        }
    };
    sweepableYoungRegionList.VisitAllRegions(promoteSweepVisitor);

    size_t newFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    size_t sweepableBytes = sweepableYoungRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    size_t promoteBytes = promoteRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    VLOG(REPORT, "select %zu B from %zu B to be region forwarded, %zu from %zu will be sweep, %zu will be promoted",
         forwardBytes, oldFromBytes, floatingGarbage, sweepableBytes, promoteBytes);
    return newFromBytes - forwardBytes;
}

class ForwardFromSpaceTask : public HeapWork {
public:
    ForwardFromSpaceTask(RegionManager& mgr, RegionList& from, GCThreadPool* pool)
        : regionManager(mgr), fromRegionList(from), threadPool(pool) {}

    ~ForwardFromSpaceTask() override = default;

    void Execute(size_t) override
    {
        constexpr size_t BATCH = 10;
        for (size_t i = 0; i < BATCH; i++) {
            RegionInfo* region = fromRegionList.TakeHeadRegion(RegionInfo::RegionType::LONE_FROM_REGION);
            if (region == nullptr) {
                return;
            }
            regionManager.ForwardRegion(region);
        }
        threadPool->AddWork(new (std::nothrow) ForwardFromSpaceTask(regionManager, fromRegionList, threadPool));
    }

private:
    RegionManager& regionManager;
    RegionList& fromRegionList;
    GCThreadPool* threadPool;
};

class PromoteRegionsTask : public HeapWork {
public:
    PromoteRegionsTask(MixedRegionManager& mgr, RegionList& promoteList, GCThreadPool* pool)
        : mixedManager(mgr), promoteRegionList(promoteList), threadPool(pool) {}

    ~PromoteRegionsTask() override = default;

    void Execute(size_t) override
    {
        constexpr size_t BATCH = 10;
        for (size_t i = 0; i < BATCH; i++) {
            RegionInfo* region = promoteRegionList.TakeHeadRegion(RegionInfo::RegionType::LONE_FROM_REGION);
            if (region == nullptr) {
                return;
            }
            mixedManager.PromoteRegion(region);
        }
        threadPool->AddWork(new (std::nothrow) PromoteRegionsTask(mixedManager, promoteRegionList, threadPool));
    }

private:
    MixedRegionManager& mixedManager;
    RegionList& promoteRegionList;
    GCThreadPool* threadPool;
};

bool MixedRegionManager::RoutePromoteRegion(RegionInfo* fromRegion)
{
    do {
        RegionInfo::RouteState oldState = fromRegion->GetRouteState();
        if (oldState == RegionInfo::RouteState::PROMOTED) {
            return true;
        }
        if (oldState == RegionInfo::RouteState::HALF_PROMOTED) {
            return false;
        }
        if (oldState == RegionInfo::RouteState::ROUTED || oldState == RegionInfo::RouteState::COMPACTED) {
            LOG(RTLOG_FATAL, "promote a wrong from region");
        }
        if (oldState == RegionInfo::RouteState::ROUTING) {
            sched_yield();
            continue;
        }
        if (fromRegion->TryLockRouting(oldState)) {
            auto ret = PromoteRegionImpl(fromRegion);
            if (ret) {
                fromRegion->SetRouteState(RegionInfo::RouteState::PROMOTED);
            } else {
                fromRegion->SetRouteState(RegionInfo::RouteState::HALF_PROMOTED);
            }
            return ret;
        }
    } while (true);
}

void MixedRegionManager::PromoteRegion(RegionInfo* fromRegion)
{
    if (RoutePromoteRegion(fromRegion)) {
        DLOG(PROMOTE, "collect promoted region %p: [%p, %p)", fromRegion, fromRegion->GetRegionStart(), fromRegion->GetRegionEnd());
        CollectRegion(fromRegion);
        // fromRegion->SetRegionType(RegionInfo::RegionType::RECENT_FULL_REGION);
    } else {
        DLOG(PROMOTE, "reactivate promoted region %p: [%p, %p)", fromRegion, fromRegion->GetRegionStart(), fromRegion->GetRegionEnd());
        recentFullRegionList.PrependRegion(fromRegion, RegionInfo::RegionType::RECENT_FULL_REGION);
    }
}

bool MixedRegionManager::PromoteRegionImpl(RegionInfo* fromRegion)
{
    CHECK(fromRegion->IsRoutingState());
    DLOG(PROMOTE, "promote region %p [%p, %p]", fromRegion, fromRegion->GetRegionStart(), fromRegion->GetRegionEnd());
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    ObjectForwardingTable* fwdTable = fromRegion->GetForwardingTable();
    if (fwdTable == nullptr) {
        fwdTable = new ObjectForwardingTable();
        fromRegion->SetForwardingTable(fwdTable);
    }
    bool promoteAll = true;
    fromRegion->VisitLiveObjectsUntilFalse([fromRegion, &collector, &fwdTable, &promoteAll](BaseObject* currentObj) {
        StateWord oldWord = currentObj->GetStateWord();
        if (currentObj->IsForwarded()) {
            return true;
        }
        while (!currentObj->TryLockObject(oldWord)) {
            (void)sched_yield();
            continue;
        }
        uint8_t age = fromRegion->GetObjectAge(currentObj);
        GenerationAllocBuffer* buffer = GenerationAllocBuffer::GetGenerationAllocBuffer();
        size_t allocSize = RegionSpace::GetAllocSize(*currentObj);
        bool needPromote = age >= PROMOTE_AGE;
        MAddress toAddress = needPromote ? buffer->AllocateOld(allocSize) : buffer->AllocateYoung(allocSize);
        if (toAddress == 0) {
            BaseObject* toObj = currentObj;
            toObj->SetStateCode(ObjectState::NORMAL);
            fwdTable->SetForwardingAddr(currentObj, toObj);
            promoteAll = false;
            DLOG(PROMOTE, "promote obj %p<%p>(%zu) to self %p", currentObj, currentObj->GetTypeInfo(), allocSize,
                toObj);
            return true;
        }
        BaseObject* toObj = reinterpret_cast<BaseObject*>(toAddress);
        DLOG(PROMOTE, "promote obj %p<%p>(%zu) to %s %p", currentObj, currentObj->GetTypeInfo(), allocSize,
             needPromote ? "to old" : "to young", toObj);
        collector.CopyObject(*currentObj, *toObj, allocSize);
        toObj->SetStateCode(ObjectState::NORMAL);
        if (needPromote) {
            RegionInfo* sourceRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(toObj));
            toObj->ForEachRefField([sourceRegion](RefField<>& field) {
                RefField<> tmp(field);
                BaseObject* target = field.GetTargetObject();
                if (!Heap::IsHeapAddress(target)) {
                    return true;
                }
                bool isTagged = tmp.IsTagged();
                RegionInfo* refRegion = nullptr;
                bool isOld = false;
                if (isTagged) {
                    refRegion = RegionInfo::GetGhostFromRegionAt(target);
                    if (refRegion != nullptr) {
                        isOld = refRegion->IsOld0();
                    } else {
                        refRegion = RegionInfo::GetRegionInfoAt(target);
                        isOld = refRegion->IsOldRegion();
                    }
                } else {
                    refRegion = RegionInfo::GetRegionInfoAt(target);
                    isOld = refRegion->IsOldRegion();
                }
                // The object that is being promoted in current gc is also recorded, since we need to fix its pointer.
                if (!isOld) {
                    RememberSet::Instance().Record(sourceRegion, reinterpret_cast<uintptr_t>(&field));
                }
                return true;
            });
        }
        currentObj->SetStateCode(ObjectState::FORWARDED);
        fwdTable->SetForwardingAddr(currentObj, toObj);
        return true;
    });
    if (!promoteAll) {
        fromRegion->SetRegionType(RegionInfo::RegionType::RECENT_FULL_REGION);
    }
    return promoteAll;
}

void MixedRegionManager::ForwardFromSpace(GCThreadPool* threadPool)
{
    if (threadPool == nullptr) {
        ForwardFromRegions();
        while (true) {
            RegionInfo* region = promoteRegionList.TakeHeadRegion(RegionInfo::RegionType::LONE_FROM_REGION);
            if (region == nullptr) {
                break;
            }
            PromoteRegion(region);
        }
        return;
    }
    threadPool->Start();
    threadPool->AddWork(new (std::nothrow) ForwardFromSpaceTask(*this, fromRegionList, threadPool));
    threadPool->AddWork(new (std::nothrow) PromoteRegionsTask(*this, promoteRegionList, threadPool));
    threadPool->WaitFinish();

    Heap::GetHeap().GetAllocator().VisitAllocBuffers([this](AllocBuffer& buffer) {
        RegionInfo* oldRegion = static_cast<GenerationAllocBuffer&>(buffer).TakeOldAllocRegion();
        if (oldRegion != nullptr && oldRegion != RegionInfo::NullRegion()) {
            RemoveThreadLocalRegion(oldRegion);
            EnlistOldRegionList(oldRegion);
            oldRegion = RegionInfo::NullRegion();
        }
    });
}

bool MixedRegionManager::IsYoungSpaceHeavy() const
{
    // return Heap::GetHeap().IsGcStarted();
    return true;
}

BaseObject* MixedRegionManager::PromoteObject(BaseObject* fromObj, RegionInfo* fromRegion)
{
    CHECK(fromRegion->IsPromoted0());
    RoutePromoteRegion(fromRegion);
    ObjectForwardingTable* fwdTable = fromRegion->GetForwardingTable();
    return fwdTable->GetRoute(fromObj);
}

} // namespace MapleRuntime
