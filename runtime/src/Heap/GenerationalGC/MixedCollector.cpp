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

void MixedCollector::DoGarbageCollection() {}

// TODO: override
bool MixedCollector::MarkObject(BaseObject* obj) const
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    size_t objectSize = obj->GetSize();
    bool marked = region->MarkObject(obj, objectSize);
    if (!marked) {
        region->AddLiveByteCount(objectSize);
        DLOG(TRACE, "mark obj %p<%p>(%zu) in region %p(%u)@%#zx, live %u", obj, obj->GetTypeInfo(), objectSize,
             region, region->GetRegionType(), region->GetRegionStart(), region->GetLiveByteCount());
    }
    return marked;
}

void MixedCollector::EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const {}

// TODO: override
void MixedCollector::EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet) const
{
    RefField<>& refField = reinterpret_cast<RefField<>&>(ref);
    RefField<> oldField(refField);
    CHECK_DETAIL(!IsOldPointer(oldField), "EnumAndTagRawRoot failed: Invalid root: %zx", oldField.GetFieldValue());
    if (IsCurrentPointer(oldField)) {
        rootSet.push_back(oldField.GetTargetObject());
        return;
    }
    BaseObject* root = oldField.GetTargetObject();
    if (Heap::IsHeapAddress(root)) {
        CHECK_DETAIL(root->IsValidObject(), "Enum and tag runtime root %p(%p) encounters invalid object", root, &ref);
        RefField<> newField = GetAndTryTagRefField(root);
        if (oldField.GetFieldValue() == newField.GetFieldValue()) {
            DLOG(ENUM, "enum raw root @%p: %p(%zu)", &ref, root, root->GetSize());
        } else if (refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
            DLOG(ENUM, "enum static ref@%p: %#zx=>%#zx -> %p<%p>(%zu)", &refField, oldField.GetFieldValue(),
                 newField.GetFieldValue(), root, root->GetTypeInfo(), root->GetSize());
        } else {
            DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &refField, oldField.GetFieldValue(), root,
                 root->GetTypeInfo(), root->GetSize());
        }
        rootSet.push_back(root);
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
    return nullptr;
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
    OldSweepTask(RegionList& sweepable, RegionList& swept, GCThreadPool* pool)
        : sweepableRegionList(sweepable), sweptRegionList(swept), threadPool(pool) {}

    ~OldSweepTask() override = default;

    void Execute(size_t) override
    {
        constexpr size_t BATCH = 10;
        for (size_t i = 0; i < BATCH; i++) {
            RegionInfo* region = sweepableRegionList.TakeHeadRegion();
            if (region == nullptr) {
                return;
            }
            region->SweepRegion();
            sweptRegionList.PrependRegion(region, RegionInfo::RegionType::SWEEP_REGION);
        }
        threadPool->AddWork(new (std::nothrow) OldSweepTask(sweepableRegionList, sweptRegionList, threadPool));
    }

private:
    RegionList& sweepableRegionList;
    RegionList& sweptRegionList;
    GCThreadPool* threadPool;
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
                                                       manager.GetOldSweptRegionList(), threadPool));
    threadPool->WaitFinish();
}

void MixedCollector::ResetForwardingData()
{
    fwdTable.PrepareForwardTable(true);
}

} // namespace MapleRuntime
