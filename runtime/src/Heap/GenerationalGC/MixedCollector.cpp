// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information


#include "MixedCollector.h"
#include "Allocator/MixedRegionSpace/MixedRegionSpace.h"
#include "Heap/GcThreadPool.h"
#include "Heap/HeapWork.h"
#include "Heap.h"
#include "RememberSet.h"

namespace MapleRuntime {

void MixedCollector::DoGarbageCollection()
{
    TraceHeap();
    PostTrace();
    ConcurrentSweep();
    Preforward();

    ForwardFromSpace();
    TransitionToGCPhase(GCPhase::GC_PHASE_GENERATION_IDLE, true);
    MergeResurrectExportObjects();
    PostResolveCycleTask();
    FlipTagID();
    ForwardDataManager::GetForwardDataManager().SetTagID(currentTagID);

    CollectSmallSpace();
    ForwardDataManager::GetForwardDataManager().UnbindPreviousLiveInfo();
}

void MixedCollector::EnumRefFieldRoot(RefField<>& field, RootSet& rootSet) const
{
    RefField<> oldField(field);
    // if field is already tagged currently, it is also already enumerated.
    if (IsCurrentPointer(oldField)) {
        rootSet.push_back(oldField.GetTargetObject());
        return;
    }

    BaseObject* latest = nullptr;
    if (IsOldPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        latest = FindLatestVersion(targetObj);
    } else {
        latest = field.GetTargetObject();
    }

    // target object could be null or non-heap for some static variable.
    if (!IsTargetAddress(latest)) {
        DLOG(ENUM, "skip invalid mixed field ref %p: %#zx -> %p", &field, oldField.GetFieldValue(), latest);
        return;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Enum static root %p(%p) encounters invalid object", latest, &field);
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(ENUM, "enum mixed static ref@%p: %#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(), latest,
             latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(ENUM, "enum mixed static ref@%p: %#zx=>%#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(),
             newField.GetFieldValue(), latest, latest->GetTypeInfo(), latest->GetSize());
    } else {
        DLOG(ENUM, "enum mixed static ref@%p: %#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(), latest,
             latest->GetTypeInfo(), latest->GetSize());
    }
    rootSet.push_back(latest);
}

// TODO: override
void MixedCollector::EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet) const
{
    RefField<>& refField = reinterpret_cast<RefField<>&>(ref);
    RefField<> oldField(refField);
    CHECK_DETAIL(!IsOldPointer(oldField), "EnumAndTagRawRoot failed: Invalid root: %zx", oldField.GetFieldValue());
    if (IsCurrentPointer(oldField)) {
        rootSet.push_back(oldField.GetTargetObject());
        DLOG(ENUM, "[MIXED] enum raw root @%p: %#zx %p(%zu)", &ref, oldField.GetFieldValue(), oldField.GetTargetObject());
        return;
    }
    BaseObject* root = oldField.GetTargetObject();
    if (Heap::IsHeapAddress(root)) {
        CHECK_DETAIL(root->IsValidObject(), "[MIXED] Enum and tag runtime root %p(%p) encounters invalid object", root, &ref);
        RefField<> newField = GetAndTryTagRefField(root);
        if (oldField.GetFieldValue() == newField.GetFieldValue()) {
            DLOG(ENUM, "[MIXED] enum raw root @%p: %p(%zu)", &ref, root, root->GetSize());
        } else if (refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
            DLOG(ENUM, "[MIXED] enum raw ref@%p: %#zx=>%#zx -> %p<%p>(%zu)", &refField, oldField.GetFieldValue(),
                 newField.GetFieldValue(), root, root->GetTypeInfo(), root->GetSize());
        } else {
            DLOG(ENUM, "[MIXED] enum raw ref@%p: %#zx -> %p<%p>(%zu)", &refField, oldField.GetFieldValue(), root,
                 root->GetTypeInfo(), root->GetSize());
        }
        rootSet.push_back(root);
    } else {
        DLOG(ENUM, "[MIXED] skip young invalid ref@%p: %#zx -> %p", &refField, oldField.GetFieldValue(), root);
    }
}

// TODO: override
RefField<> MixedCollector::GetAndTryTagRefField(BaseObject* target) const
{
    if (IsFromObject(target)) {
        return RefField<>(target, 1, currentTagID);
    } else {
        return RefField<>(target);
    }
}

BaseObject* MixedCollector::ForwardObject(BaseObject* fromVersion)
{
    return YoungCollector::ForwardObject(fromVersion);
}

bool MixedCollector::IsTargetAddress(BaseObject* obj) const
{
    return Heap::IsHeapAddress(obj);
}

void MixedCollector::AssembleCandidates()
{
    reinterpret_cast<MixedRegionSpace&>(theAllocator).AssembleMixedCandidates();
}

class OldSweepTask : public HeapWork {
public:
    OldSweepTask(RegionList& sweepable, RegionList& swept, GCThreadPool* pool, RegionManager& manager)
        : sweepableRegionList(sweepable), sweptRegionList(swept), threadPool(pool), regionManager(manager) {}

    ~OldSweepTask() override = default;

    void Execute(size_t) override
    {
        constexpr size_t BATCH = 10;
        for (size_t i = 0; i < BATCH; i++) {
            RegionInfo* region = sweepableRegionList.TakeHeadRegion();
            if (region == nullptr) {
                return;
            }
            if (region->GetLiveByteCount() == 0) {
                region->ResetRSetCardTable();
                regionManager.CollectRegion(region);
                continue;
            }
            region->SweepRegion();
            sweptRegionList.PrependRegion(region, RegionInfo::RegionType::SWEEP_REGION);
        }
        threadPool->AddWork(new (std::nothrow) OldSweepTask(sweepableRegionList, sweptRegionList, threadPool,
                                                           regionManager));
    }

private:
    RegionList& sweepableRegionList;
    RegionList& sweptRegionList;
    GCThreadPool* threadPool;
    RegionManager& regionManager;
};

void MixedCollector::ConcurrentSweep()
{
    YoungCollector::ConcurrentSweep();
    GCThreadPool* threadPool = GetThreadPool();
    if (threadPool == nullptr) {
        return;
    }
    MixedRegionSpace& space = static_cast<MixedRegionSpace&>(theAllocator);
    MixedRegionManager& manager = MixedRegionSpace::CastDown(space.GetRegionManager());
    threadPool->AddWork(new (std::nothrow) OldSweepTask(manager.GetSweepableOldRegionList(),
                                                       manager.GetOldSweptRegionList(), threadPool, manager));
}

void MixedCollector::ResetForwardingData()
{
    fwdTable.PrepareForwardTable(true);
}

} // namespace MapleRuntime
