// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
#include "MinorCollector.h"
#include "Collector/CopyCollector.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/ForwardDataManager.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {

void MinorCollector::Init()
{
    DLOG(GC, "MinorCollector::Init");
}

void MinorCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    minorGCIndex = gcIndex;
    SetGCPhase(GCPhase::GC_PHASE_INIT);

    DLOG(GC, "Start Minor GC #%zu", gcIndex);

    WorkStack workStack = NewWorkStack();

    ScanRoots(workStack);

    SetGCPhase(GCPhase::GC_PHASE_TRACE);
    ForwardSurvivors(workStack);

    SetGCPhase(GCPhase::GC_PHASE_FINISH);
    DLOG(GC, "End Minor GC #%zu, survived %zu bytes", gcIndex, survivedSize);
}

void MinorCollector::ScanRoots(WorkStack& workStack)
{
    MutatorManager::Instance().IterateMutators([&workStack](Mutator* mutator) {
        mutator->VisitRoots([&workStack](BaseObject* obj) {
            if (obj != nullptr) {
                workStack.push_back(obj);
            }
        });
    });

    theAllocator.ForEachObj([&workStack](BaseObject* obj) {
        RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        if (regionInfo->IsOldGeneration()) {
            workStack.push_back(obj);
        }
    }, false);
}

void MinorCollector::ForwardSurvivors(WorkStack& workStack)
{
    while (!workStack.empty()) {
        BaseObject* obj = workStack.back();
        workStack.pop_back();

        RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        if (!regionInfo->IsYoungGeneration()) {
            continue;
        }

        size_t offset = regionInfo->GetAddressOffset(reinterpret_cast<MAddress>(obj));
        if (regionInfo->IsMarkedObject(offset)) {
            continue;
        }

        regionInfo->MarkObject(obj);
        size_t objSize = obj->GetSize();
        regionInfo->AddLiveByteCount(objSize);
        survivedSize += objSize;

        ForwardObject(obj);

        if (ShouldPromote(obj)) {
            PromoteToOldGen(obj);
        } else {
            regionInfo->IncrementAge();
        }
    }
}

bool MinorCollector::ShouldPromote(BaseObject* obj)
{
    RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    return regionInfo->GetAge() >= genConfig.maxAge;
}

void MinorCollector::PromoteToOldGen(BaseObject* obj)
{
    RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    regionInfo->SetGeneration(RegionInfo::Generation::GEN_OLD);
    regionInfo->ResetAge();
}

void MinorCollector::SwapSurvivorSpaces()
{
    DLOG(GC, "Swap survivor spaces");
}

BaseObject* MinorCollector::ForwardObject(BaseObject* obj)
{
    size_t objSize = obj->GetSize();
    MAddress newAddr = theAllocator.Allocate(objSize, AllocType::REGULAR);
    if (newAddr == 0) {
        return obj;
    }

    RegionInfo* toRegion = RegionInfo::GetRegionInfoAt(newAddr);
    toRegion->SetGeneration(RegionInfo::Generation::GEN_YOUNG);

    auto* toObj = reinterpret_cast<BaseObject*>(newAddr);
    memcpy(toObj, obj, objSize);

    return toObj;
}

} // namespace MapleRuntime