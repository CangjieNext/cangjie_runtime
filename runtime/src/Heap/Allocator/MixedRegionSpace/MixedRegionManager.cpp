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
size_t MixedRegionManager::ClassifyYoungRegions()
{
    size_t forwardBytes = 0;
    size_t floatingGarbage = 0;
    size_t oldFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    double exempt = exemptedRegionThreshold;
    auto visitor = [this, exempt, &floatingGarbage](RegionInfo* fromRegion) {
        long rawPtrCnt = fromRegion->GetRawPointerObjectCount();
        if (rawPtrCnt == 0 && fromRegion->IsPrompted()) {
            RemoveRegionLocked(&fromRegionList, fromRegion);
            promptRegionList.PrependRegion(fromRegion, RegionInfo::RegionType::FROM_REGION);
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
            sweepableYoungRegionList.PrependRegion(del, RegionInfo::RegionType::SWEEP_REGION);
            floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
        }
    };
    fromRegionList.VisitAllRegions(visitor);

    auto promptSweepVisitor = [this](RegionInfo* region) {
        long rawPtrCnt = region->GetRawPointerObjectCount();
        if (rawPtrCnt == 0 && region->IsPrompted()) {
            RemoveRegionLocked(&sweepableYoungRegionList, region);
            recentFullRegionList.PrependRegion(region, RegionInfo::RegionType::RECENT_FULL_REGION);
        }
    };
    sweepableYoungRegionList.VisitAllRegions(promptSweepVisitor);

    size_t newFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    size_t exemptedFromBytes = unmovableFromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    VLOG(REPORT, "exempt from-space: %zu B - %zu B -> %zu B, %zu B floating garbage, %zu B to forward",
         oldFromBytes, exemptedFromBytes, newFromBytes, floatingGarbage, forwardBytes);
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

class PromptRegionsTask : public HeapWork {
public:
    PromptRegionsTask(MixedRegionManager& mgr, RegionList& promptList, GCThreadPool* pool)
        : mixedManager(mgr), promptRegionList(promptList), threadPool(pool) {}

    ~PromptRegionsTask() override = default;

    void Execute(size_t) override
    {
        constexpr size_t BATCH = 10;
        for (size_t i = 0; i < BATCH; i++) {
            RegionInfo* region = promptRegionList.TakeHeadRegion();
            if (region == nullptr) {
                return;
            }
            mixedManager.PromptRegion(region);
        }
        threadPool->AddWork(new (std::nothrow) PromptRegionsTask(mixedManager, promptRegionList, threadPool));
    }

private:
    MixedRegionManager& mixedManager;
    RegionList& promptRegionList;
    GCThreadPool* threadPool;
};

bool MixedRegionManager::PromptRegion(RegionInfo* fromRegion)
{
    do {
        RegionInfo::RouteState oldState = fromRegion->GetRouteState();
        if (oldState == RegionInfo::RouteState::PROMPTED) {
            return true;
        }
        if (oldState == RegionInfo::RouteState::ROUTED || oldState == RegionInfo::RouteState::COMPACTED) {
            LOG(RTLOG_FATAL, "prompt a wrong from region");
        }
        if (oldState == RegionInfo::RouteState::ROUTING) {
            sched_yield();
            continue;
        }
        if (fromRegion->TryLockRouting(oldState)) {
            PromptRegionImpl(fromRegion);
            fromRegion->SetRouteState(RegionInfo::RouteState::PROMPTED);
            return true;
        }
    } while (true);
}

void MixedRegionManager::PromptRegionImpl(RegionInfo* fromRegion)
{
    CHECK(fromRegion->IsRoutingState());
    DLOG(PROMPT, "promt region %p [%p, %p]", fromRegion, fromRegion->GetRegionStart(), fromRegion->GetRegionEnd());
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    ObjectForwardingTable* fwdTable = fromRegion->GetForwardingTable();
    if (fwdTable == nullptr) {
        fwdTable = new ObjectForwardingTable();
        fromRegion->SetForwardingTable(fwdTable);
    }
    fromRegion->VisitLiveObjectsUntilFalse([fromRegion, &collector, &fwdTable](BaseObject* currentObj) {
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
        bool needPrompt = age >= PROMPT_AGE;
        MAddress toAddress = needPrompt ? buffer->AllocateOld(allocSize) : buffer->AllocateYoung(allocSize);
        BaseObject* toObj = reinterpret_cast<BaseObject*>(toAddress);
        collector.CopyObject(*currentObj, *toObj, allocSize);
        toObj->SetStateCode(ObjectState::NORMAL);
        if (needPrompt) {
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
                // The object that being prompted in current gc is also recorded, since we need to fix pointer.
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

}

void MixedRegionManager::ForwardFromSpace(GCThreadPool* threadPool)
{
    if (threadPool == nullptr) {
        ForwardFromRegions();
        while (true) {
            RegionInfo* region = promptRegionList.TakeHeadRegion();
            if (region == nullptr) {
                break;
            }
            PromptRegion(region);
        }
        return;
    }
    threadPool->Start();
    threadPool->AddWork(new (std::nothrow) ForwardFromSpaceTask(*this, fromRegionList, threadPool));
    threadPool->AddWork(new (std::nothrow) PromptRegionsTask(*this, promptRegionList, threadPool));
    threadPool->WaitFinish();

    Heap::GetHeap().GetAllocator().VisitAllocBuffers([this](AllocBuffer& buffer) {
        RegionInfo* oldRegion = static_cast<GenerationAllocBuffer&>(buffer).TakeOldAllocRegion();
        if (oldRegion != nullptr && oldRegion != RegionInfo::NullRegion()) {
            EnlistOldRegionList(oldRegion);
        }
    });
}

bool MixedRegionManager::IsYoungSpaceHeavy() const
{
    return Heap::GetHeap().IsGcStarted();
}

BaseObject* MixedRegionManager::PromptObject(BaseObject* fromObj, RegionInfo* fromRegion)
{
    CHECK(fromRegion->IsPrompted0());
    PromptRegion(fromRegion);
    ObjectForwardingTable* fwdTable = fromRegion->GetForwardingTable();
    return fwdTable->GetRoute(fromObj);
}

} // namespace MapleRuntime
