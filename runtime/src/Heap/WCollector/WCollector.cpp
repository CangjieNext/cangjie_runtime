// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "WCollector.h"

#include "Concurrency/Concurrency.h"
#include "Heap/Allocator/LOSManager.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {

std::atomic<uint32_t> g_gcCycleNum{0};

bool WCollector::IsUnmovableFromObject(BaseObject* obj) const
{
    // filter const string object.
    if (!Heap::IsHeapAddress(obj)) {
        return false;
    }

    RegionInfo* regionInfo = nullptr;
    if (RegionInfo::InGhostFromRegion(obj)) {
        regionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<uintptr_t>(obj));
    } else {
        regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    }
    // Defense-in-depth: regionInfo can be nullptr if metadata is in a
    // transient state (concurrent region init/free).  Treat as non-unmovable.
    return regionInfo != nullptr && regionInfo->IsUnmovableFromRegion();
}

bool WCollector::MarkObject(BaseObject* obj) const
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    if (UNLIKELY(region == nullptr)) {
        return true; // treat as already marked — skip tracing
    }
    size_t objectSize = obj->GetSize();
    bool marked = region->MarkObject(obj, objectSize);
    if (!marked) {
        region->AddLiveByteCount(objectSize);
        (void)region;
        DLOG(TRACE, "mark obj %p<%p>(%zu) in region %p(%u)@%#zx, live %u", obj, obj->GetTypeInfo(), objectSize,
             region, region->GetRegionType(), region->GetRegionStart(), region->GetLiveByteCount());
    }
    return marked;
}

bool WCollector::ResurrectObject(BaseObject* obj, size_t offset, RegionInfo* region)
{
    bool resurrected = region->ResurrectObject(obj, offset);
        if (!resurrected) {
            region->AddLiveByteCount(obj->GetSize());
            DLOG(TRACE, "resurrect region %p@%#zx obj %p<%p>(%zu), live bytes %u", region, region->GetRegionStart(),
                 obj, obj->GetTypeInfo(), obj->GetSize(), region->GetLiveByteCount());
        }
        return resurrected;
}

// this api updates current pointer as well as old pointer, caller should take care of this.
template<bool forward>
bool WCollector::TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
                                       BaseObject*& toObj) const
{
    RefField<> oldRef(field);
    if (oldRef.IsTagged()) {
        fromObj = oldRef.GetTargetObject();
        CHECK_DETAIL(fromObj != nullptr, "tagged ref has null target: field@%p value=%#zx", &field,
                     oldRef.GetFieldValue());

        bool isOldTagged = IsOldPointer(oldRef);
        if (isOldTagged && Heap::IsHeapAddress(fromObj) && !fromObj->IsValidObject()) {
            toObj = FindToVersion(fromObj);
            CHECK_DETAIL(toObj != nullptr,
                         "invalid old-tag target object: field@%p from=%p value=%#zx", &field, fromObj,
                         oldRef.GetFieldValue());

            RefField<> tmpField(toObj);
            if (field.CompareExchange(oldRef.GetFieldValue(), tmpField.GetFieldValue())) {
                if (obj != nullptr) {
                    DLOG(TRACE, "update(old-nonobj) obj %p<%p>+%zu ref-field@%p: %#zx -> %#zx", obj,
                         obj->GetTypeInfo(), BaseObject::FieldOffset(obj, &field), &field,
                         oldRef.GetFieldValue(), tmpField.GetFieldValue());
                } else {
                    DLOG(TRACE, "update(old-nonobj) ref@%p: %#zx -> %#zx", &field,
                         oldRef.GetFieldValue(), tmpField.GetFieldValue());
                }
            }
            return true;
        }

        if (forward) {
            toObj = const_cast<WCollector*>(this)->TryForwardObject(fromObj);
        } else {
            toObj = FindToVersion(fromObj);
        }
        if (toObj == nullptr) {
            return false;
        }
        RefField<> tmpField(toObj);
        if (field.CompareExchange(oldRef.GetFieldValue(), tmpField.GetFieldValue())) {
            if (obj != nullptr) {
                DLOG(TRACE, "update obj %p<%p>+%zu ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(),
                     BaseObject::FieldOffset(obj, &field), &field, oldRef.GetFieldValue(),
                     tmpField.GetFieldValue());
            } else {
                DLOG(TRACE, "update ref@%p: 0x%zx -> %p", &field, oldRef.GetFieldValue(), toObj);
            }
            return true;
        } else {
            if (obj != nullptr) {
                DLOG(TRACE, "update obj %p<%p>+%zu but cas failed ref-field@%p: %#zx(%#zx) -> %#zx", obj,
                     obj->GetTypeInfo(), BaseObject::FieldOffset(obj, &field), &field, oldRef.GetFieldValue(),
                     field.GetFieldValue(), tmpField.GetFieldValue());
            } else {
                DLOG(TRACE, "update but cas failed ref@%p: 0x%zx(%zx) -> %p", &field, oldRef.GetFieldValue(),
                     field.GetFieldValue(), toObj);
            }
            return true;
        }
    }

    return false;
}
bool WCollector::TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
{
    BaseObject* oldRef = nullptr;
    return TryUpdateRefFieldImpl<false>(obj, field, oldRef, newRef);
}

bool WCollector::TryForwardRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
{
    BaseObject* oldRef = nullptr;
    return TryUpdateRefFieldImpl<true>(obj, field, oldRef, newRef);
}
// this api untags current pointer as well as old pointer, caller should take care of this.
bool WCollector::TryUntagRefField(BaseObject* obj, RefField<>& field, BaseObject*& target) const
{
    for (;;) {
        RefField<> oldRef(field);
        if (!oldRef.IsTagged()) {
            return false;
        }
        target = oldRef.GetTargetObject();
        RefField<> newRef(target);
        if (field.CompareExchange(oldRef.GetFieldValue(), newRef.GetFieldValue())) {
            if (obj != nullptr) {
                DLOG(FIX, "untag obj %p<%p>(%zu) ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(), obj->GetSize(),
                     &field, oldRef.GetFieldValue(), newRef.GetFieldValue());
            } else {
                DLOG(FIX, "untag ref@%p: %#zx -> %#zx", &field, oldRef.GetFieldValue(), newRef.GetFieldValue());
            }
            return true;
        }
    }

    return false;
}

// RefFieldRoot is root in tagged pointer format.
void WCollector::EnumRefFieldRoot(RefField<>& field, RootSet& rootSet) const
{
    RefField<> oldField(field);
    // if field is already tagged currently, it is also already enumerated.
    if (IsCurrentPointer(oldField)) {
        rootSet.push_back(oldField.GetTargetObject());
        return;
    }

    BaseObject* latest = nullptr;
    BaseObject* diagTargetObj = nullptr;
    if (IsOldPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        diagTargetObj = targetObj;
        latest = FindLatestVersion(targetObj);
    } else {
        latest = field.GetTargetObject();
    }

    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    if (UNLIKELY(!latest->IsValidObject())) {
        uint32_t crashCycleNum = g_gcCycleNum.load();
        RegionInfo* diagRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(latest));
        MAddress fieldVal = oldField.GetFieldValue();
        uint16_t fIsTagged = oldField.IsTagged() ? 1 : 0;
        uint16_t fTagID = oldField.GetTagID();
        unsigned rType = diagRegion ? static_cast<unsigned>(diagRegion->GetRegionType()) : 0u;

        // Also get from-version region info
        RegionInfo* fromRegion = diagTargetObj ?
            RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(diagTargetObj)) : nullptr;
        unsigned fromRType = fromRegion ? static_cast<unsigned>(fromRegion->GetRegionType()) : 0u;
        RegionInfo* ghostFrom = diagTargetObj ?
            RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(diagTargetObj)) : nullptr;

        LOG(RTLOG_ERROR, "[cycle=%u] Enum static root latest=%p targetObj=%p(@field=%p fieldVal=%#zx tagged=%u tagID=%u) "
            "invalid object. "
            "toRegion=%p toRegionType=%u toIsFree=%d toIsGarbage=%d toIsGhostFrom=%d "
            "toRegionStart=%#zx toRegionEnd=%#zx toAllocPtr=%#zx "
            "fromRegion=%p fromRegionType=%u fromIsFree=%d fromIsGarbage=%d fromIsGhostFrom=%d "
            "fromGhostRegion=%p "
            "fromRegionStart=%#zx fromRegionEnd=%#zx fromLiveBytes=%u "
            "curTagID=%u prevTagID=%u gcReason=%d",
            crashCycleNum,
            latest, diagTargetObj, &field, fieldVal, fIsTagged, fTagID,
            diagRegion, rType,
            diagRegion ? (int)diagRegion->IsFreeRegion() : -1,
            diagRegion ? (int)diagRegion->IsGarbageRegion() : -1,
            diagRegion ? (int)diagRegion->IsGhostFromRegion() : -1,
            diagRegion ? diagRegion->GetRegionStart() : 0,
            diagRegion ? diagRegion->GetRegionEnd() : 0,
            diagRegion ? diagRegion->GetRegionAllocPtr() : 0,
            fromRegion, fromRType,
            fromRegion ? (int)fromRegion->IsFreeRegion() : -1,
            fromRegion ? (int)fromRegion->IsGarbageRegion() : -1,
            fromRegion ? (int)fromRegion->IsGhostFromRegion() : -1,
            ghostFrom,
            fromRegion ? fromRegion->GetRegionStart() : 0,
            fromRegion ? fromRegion->GetRegionEnd() : 0,
            fromRegion ? fromRegion->GetLiveByteCount() : 0,
            (unsigned)currentTagID, (unsigned)(currentTagID ^ 1), gcReason);

        // Dump ghost routing details if available
        if (ghostFrom != nullptr) {
            auto& rInfo = ghostFrom->GetRouteInfoRef();
            LiveInfo* li0 = ghostFrom->GetLiveInfo0();
            uint64_t preLive = 0;
            if (li0 != nullptr && diagTargetObj != nullptr) {
                size_t offset = reinterpret_cast<MAddress>(diagTargetObj) - ghostFrom->GetRegionStart();
                preLive = li0->GetPreLiveBytes(offset, ghostFrom->GetGhostRegionSize());
            }
            LOG(RTLOG_ERROR, "  ghostRoute: routeState=%u liveInfo0=%p "
                "routeInfo.to1Start=%#zx routeInfo.to1Used=%u routeInfo.to2Idx=%u "
                "preLiveBytes=%llu ghostRegionSize=%zu ghostRegionEnd0=%#zx "
                "ghostUnitRole0=%u",
                (unsigned)ghostFrom->GetRouteState(), li0,
                rInfo.toRegion1StartAddress, rInfo.toRegion1UsedBytes, rInfo.toRegion2Idx,
                (unsigned long long)preLive, ghostFrom->GetGhostRegionSize(),
                ghostFrom->GetGhostRegionEnd0(),
                (unsigned)ghostFrom->GetUnitRole0());
        }

        // Dump raw memory at 'latest' (to-version) — first 32 bytes
        {
            const uint64_t* mem = reinterpret_cast<const uint64_t*>(latest);
            LOG(RTLOG_ERROR, "  toMem @%p: [0]=%#zx [1]=%#zx [2]=%#zx [3]=%#zx",
                latest, mem[0], mem[1], mem[2], mem[3]);
        }

        // Also check from-version data
        if (diagTargetObj != nullptr) {
            const uint64_t* fromMem = reinterpret_cast<const uint64_t*>(diagTargetObj);
            LOG(RTLOG_ERROR, "  fromMem @%p: [0]=%#zx [1]=%#zx [2]=%#zx [3]=%#zx",
                diagTargetObj, fromMem[0], fromMem[1], fromMem[2], fromMem[3]);
        }

        std::abort();
    }
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
           DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>", &field, oldField.GetFieldValue(), latest,
               latest->GetTypeInfo());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
           DLOG(ENUM, "enum static ref@%p: %#zx=>%#zx -> %p<%p>", &field, oldField.GetFieldValue(),
               newField.GetFieldValue(), latest, latest->GetTypeInfo());
    } else {
           DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>", &field, oldField.GetFieldValue(), latest,
               latest->GetTypeInfo());
    }
    rootSet.push_back(latest);
}

void WCollector::EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet) const
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
            DLOG(ENUM, "enum raw root @%p: %p", &ref, root);
        } else if (refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
              DLOG(ENUM, "enum static ref@%p: %#zx=>%#zx -> %p<%p>", &refField, oldField.GetFieldValue(),
                  newField.GetFieldValue(), root, root->GetTypeInfo());
        } else {
              DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>", &refField, oldField.GetFieldValue(), root,
                  root->GetTypeInfo());
        }
        rootSet.push_back(root);
    }
}

// note each ref-field will not be traced twice, so each old pointer the tracer meets must come from previous gc.
void WCollector::TraceRefField(BaseObject* obj, RefField<>& field, WorkStack& workStack) const
{
    RefField<> oldField(field);
    if (IsCurrentPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        if (!IsMarkedObject(targetObj)) {
            workStack.push_back(targetObj);
        }
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
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    if (UNLIKELY(!latest->IsValidObject())) {
        RegionInfo* diagRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(latest));
        MAddress fieldVal = oldField.GetFieldValue();
        unsigned rType2 = diagRegion ? static_cast<unsigned>(diagRegion->GetRegionType()) : 0u;
        BaseObject* traceTargetObj = IsOldPointer(oldField) ? oldField.GetTargetObject() : nullptr;
        RegionInfo* traceFromRegion = traceTargetObj ?
            RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(traceTargetObj)) : nullptr;
        RegionInfo* traceGhostFrom = traceTargetObj ?
            RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(traceTargetObj)) : nullptr;

        LOG(RTLOG_ERROR, "Trace ref invalid: latest=%p obj=%p objType=%s offset=%zd "
            "fieldVal=%#zx tagged=%u tagID=%u "
            "toRegion=%p toRegionType=%u toIsFree=%d toIsGarbage=%d toIsGhostFrom=%d "
            "toRegionStart=%#zx toRegionEnd=%#zx toAllocPtr=%#zx "
            "fromObj=%p fromRegion=%p fromRegionType=%u fromIsGhostFrom=%d "
            "fromGhostRegion=%p fromLiveBytes=%u "
            "curTagID=%u prevTagID=%u gcReason=%d",
            latest, obj, obj->GetTypeInfo()->GetName(),
            BaseObject::FieldOffset(obj, &field),
            fieldVal,
            oldField.IsTagged() ? 1u : 0u, (unsigned)oldField.GetTagID(),
            diagRegion, rType2,
            diagRegion ? (int)diagRegion->IsFreeRegion() : -1,
            diagRegion ? (int)diagRegion->IsGarbageRegion() : -1,
            diagRegion ? (int)diagRegion->IsGhostFromRegion() : -1,
            diagRegion ? diagRegion->GetRegionStart() : 0,
            diagRegion ? diagRegion->GetRegionEnd() : 0,
            diagRegion ? diagRegion->GetRegionAllocPtr() : 0,
            traceTargetObj,
            traceFromRegion, traceFromRegion ? static_cast<unsigned>(traceFromRegion->GetRegionType()) : 0u,
            traceFromRegion ? (int)traceFromRegion->IsGhostFromRegion() : -1,
            traceGhostFrom,
            traceFromRegion ? traceFromRegion->GetLiveByteCount() : 0,
            (unsigned)currentTagID, (unsigned)(currentTagID ^ 1), gcReason);

        // Dump ghost routing details if available
        if (traceGhostFrom != nullptr) {
            auto& rInfo = traceGhostFrom->GetRouteInfoRef();
            LiveInfo* li0 = traceGhostFrom->GetLiveInfo0();
            uint64_t preLive = 0;
            if (li0 != nullptr && traceTargetObj != nullptr) {
                size_t offset = reinterpret_cast<MAddress>(traceTargetObj) - traceGhostFrom->GetRegionStart();
                preLive = li0->GetPreLiveBytes(offset, traceGhostFrom->GetGhostRegionSize());
            }
            LOG(RTLOG_ERROR, "  ghostRoute: routeState=%u liveInfo0=%p "
                "routeInfo.to1Start=%#zx routeInfo.to1Used=%u routeInfo.to2Idx=%u "
                "preLiveBytes=%llu ghostRegionSize=%zu ghostRegionEnd0=%#zx",
                (unsigned)traceGhostFrom->GetRouteState(), li0,
                rInfo.toRegion1StartAddress, rInfo.toRegion1UsedBytes, rInfo.toRegion2Idx,
                (unsigned long long)preLive, traceGhostFrom->GetGhostRegionSize(),
                traceGhostFrom->GetGhostRegionEnd0());
        }

        // Dump raw memory at 'latest' (to-version)
        {
            const uint64_t* mem = reinterpret_cast<const uint64_t*>(latest);
            LOG(RTLOG_ERROR, "  toMem @%p: [0]=%#zx [1]=%#zx [2]=%#zx [3]=%#zx",
                latest, mem[0], mem[1], mem[2], mem[3]);
        }

        // Also dump from-version memory
        if (traceTargetObj != nullptr) {
            const uint64_t* fromMem = reinterpret_cast<const uint64_t*>(traceTargetObj);
            LOG(RTLOG_ERROR, "  fromMem @%p: [0]=%#zx [1]=%#zx [2]=%#zx [3]=%#zx",
                traceTargetObj, fromMem[0], fromMem[1], fromMem[2], fromMem[3]);
        }

        std::abort();
    }
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>", obj, &field, latest, latest->GetTypeInfo());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
           DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>", obj, &field, oldField.GetFieldValue(),
               newField.GetFieldValue(), latest, latest->GetTypeInfo());
    }

    if (!IsMarkedObject(latest)) {
        workStack.push_back(latest);
    }
}

void WCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack)
{
    auto refFunc = [this, obj, &workStack](RefField<>& field) { TraceRefField(obj, field, workStack); };

    obj->ForEachRefField(refFunc);
}

BaseObject* WCollector::GetAndTryTagObj(BaseObject* obj, RefField<>& field)
{
    RefField<> oldField(field);
    BaseObject* latest = nullptr;
    if (IsCurrentPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        return targetObj;
    }
    if (IsOldPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        latest = FindLatestVersion(targetObj);
    } else {
        latest = field.GetTargetObject();
    }
    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return nullptr;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by weak object %p: %s and offset %zd",
                 latest, obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>", obj, &field, latest, latest->GetTypeInfo());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>", obj, &field, oldField.GetFieldValue(),
            newField.GetFieldValue(), latest, latest->GetTypeInfo());
    }
    return latest;
}

BaseObject* WCollector::ForwardUpdateRawRef(ObjectRef& root)
{
    auto& refField = reinterpret_cast<RefField<>&>(root);
    RefField<> oldField(refField);
    BaseObject* oldObj = oldField.GetTargetObject();
    DLOG(FIX, "visit raw-ref @%p: %p", &root, oldObj);
    CHECK_DETAIL(!IsOldPointer(oldField), "ForwardUpdateRawRef failed: Invalid object: %zx", oldField.GetFieldValue());
    if (IsCurrentPointer(oldField)) {
        if (IsGhostFromObject(oldObj)) {
            BaseObject* toVersion = TryForwardObject(oldObj);
            CHECK(toVersion != nullptr);
            RefField<> newField(toVersion);
            // CAS failure means some mutator or gc thread writes a new ref (must be a to-object), no need to retry.
            if (refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
                DLOG(FIX, "fix raw-ref @%p: %p -> %p", &root, oldObj, toVersion);
                return toVersion;
            }
            CHECK(!IsCurrentPointer(refField));
        } else {
            RefField<> newField(oldObj);
            // CAS failure means some mutator or gc thread writes a new ref (must be a to-object), no need to retry.
            if (refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
                DLOG(FIX, "fix raw-ref @%p: %p -> %p", &root, oldObj, oldObj);
                return oldObj;
            }
        }
    }

    return oldObj;
}
void WCollector::PreforwardAllExportFromRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    Heap::GetHeap().VisitAllExportRoots(visitor);
}
void WCollector::PreforwardFinalizerProcessorRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    collectorResources.GetFinalizerProcessor().VisitRawPointers(visitor);
}

void WCollector::PreforwardConcurrencyModelRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitor);
}

void WCollector::PreforwardDiscoveredExternObjects()
{
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    CHECK(discoveredExternObjects.empty());
    auto it = cycleRefWorkStack.begin();
    std::unordered_map<BaseObject*, std::list<BaseObject*>> tmp;
    while (it != cycleRefWorkStack.end()) {
        BaseObject* exportObj = it->first;
        BaseObject* latest = exportObj;
        if (IsGhostFromObject(exportObj) && !IsUnmovableFromObject(exportObj)) {
            latest = ForwardObject(exportObj);
        }
        for (auto &externObj : it->second) {
            if (IsGhostFromObject(externObj) && !IsUnmovableFromObject(externObj)) {
                BaseObject* toObj = ForwardObject(externObj);
                externObj = toObj;
            }
        }
        if (latest != exportObj) {
            tmp[latest] = it->second;
            it = cycleRefWorkStack.erase(it);
        } else {
            it++;
        }
    }
    if (!tmp.empty()) {
        cycleRefWorkStack.insert(tmp.begin(), tmp.end());
    }
}

void WCollector::PreforwardAllResurrectExportFromObjects()
{
    std::unordered_set<BaseObject*> tmp;
    std::lock_guard<std::mutex> lg(resurrectExportMtx);
    auto it = resurrectedExportObjectes.begin();
    while (it != resurrectedExportObjectes.end()) {
        BaseObject* exportObj = *it;
        BaseObject* latest = exportObj;
        if (IsGhostFromObject(exportObj) && !IsUnmovableFromObject(exportObj)) {
            latest = ForwardObject(exportObj);
        }
        if (latest != exportObj) {
            tmp.insert(latest);
            it = resurrectedExportObjectes.erase(it);
        } else {
            it++;
        }
    }
    if (!tmp.empty()) {
        resurrectedExportObjectes.insert(tmp.begin(), tmp.end());
    }
}
void WCollector::TraceHeap()
{
    VLOG(REPORT, "GC TraceHeap: tagID=%u reason=%d", (unsigned)currentTagID, gcReason);
    WorkStack workStack = NewWorkStack();
    WorkStack foreignStack = NewWorkStack();
    // assemble garbage candidates for tracing.
    reinterpret_cast<RegionSpace&>(theAllocator).AssembleGarbageCandidates();

    {
        MRT_PHASE_TIMER("enum roots & update old pointers within");
        TransitionToGCPhase(GCPhase::GC_PHASE_ENUM, true);
        DoEnumeration(workStack, foreignStack);
    }

    {
        MRT_PHASE_TIMER("trace live objects & update old pointers in ref-fields");
        markedObjectCount.store(0, std::memory_order_relaxed);
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        DoTracing(workStack, foreignStack);

        ProcessFinalizers();
    }
}

void WCollector::PostTrace()
{
    MRT_PHASE_TIMER("PostTrace");
    VLOG(REPORT, "GC PostTrace: tagID=%u reason=%d", (unsigned)currentTagID, gcReason);
    TransitionToGCPhase(GC_PHASE_POST_TRACE, true);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    space.GetRegionManager().HandleTraceRegions();
    // clear weakRef List, set the referent as null
    WeakRefBuffer::Instance().ClearWeakRefBuffer();
    // clear satb buffer when gc finish tracing.
    SatbBuffer::Instance().ClearBuffer();
    // reclaim large objects immediately after tracing is done.
    PrepareCycleRef();
    CollectLargeGarbage();
    // CompactLOS moved to post-GC phase (alongside ShrinkInactiveZone),
    // gated by LOS fragmentation > 30% instead of OOM-only.
    CollectPinnedGarbage();
    RefineFromSpace();
    fwdTable.PrepareForwardTable();
    // M3: Build forwarding routes to migrate surviving MAIN large objects to LOS.
    // Must run AFTER PrepareForwardTable (which calls DispelGhostFromRegion for previous ghosts).
    // Candidates were pre-selected in AssembleMigrationCandidates (before trace) so that
    // references to them are properly tagged during Enum/Trace.
    BuildLargeObjectMigrationRoutes();
}

void WCollector::BuildLargeObjectMigrationRoutes()
{
    MRT_PHASE_TIMER("BuildLargeObjectMigrationRoutes");

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& rm = space.GetRegionManager();
    RegionList& migrationList = rm.GetMigrationCandidateList();

    if (migrationList.GetRegionCount() == 0) {
        return;
    }

    LOSManager* losMgr = rm.GetLOSManager();
    RegionList& fromList = rm.GetFromRegionList();

    // Snapshot candidates into a vector (we'll modify the list during iteration).
    std::vector<RegionInfo*> candidates;
    migrationList.VisitAllRegions([&](RegionInfo* region) {
        candidates.push_back(region);
    });

    size_t migratedCount = 0;
    size_t deadCount = 0;
    size_t losFullCount = 0;

    for (RegionInfo* region : candidates) {
        // Dead after trace: collect immediately.
        if (region->GetLiveByteCount() == 0) {
            migrationList.DeleteRegion(region);
            rm.CollectRegion(region);
            deadCount++;
            continue;
        }

        // Try to allocate LOS target space.
        size_t objSize = region->GetRegionAllocatedSize();
        uintptr_t toAddr = (losMgr != nullptr) ? losMgr->AllocLarge(objSize) : 0;
        if (toAddr == 0 && losMgr != nullptr) {
            // LOS full: try borrowing from RESERVE to extend LOS runway,
            // then retry allocation.  This keeps migration fully concurrent
            // (no STW needed) by reusing the ghost-routing path.
            ReserveManager* resMgr = rm.GetReserveManager();
            if (resMgr != nullptr) {
                size_t borrowed = resMgr->BorrowForLOS(objSize);
                if (borrowed > 0) {
                    toAddr = losMgr->AllocLarge(objSize);
                    VLOG(REPORT, "LOS migration: borrowed %zu from RESERVE for region %p, "
                         "retry alloc %s", borrowed, region, toAddr ? "OK" : "FAILED");
                }
            }
        }
        if (toAddr == 0) {
            // LOS + RESERVE both exhausted: mark as UNMOVABLE_FROM_REGION so
            // the ForwardBarrier untags the reference without ghost routing.
            // The region stays in migrationCandidateList and is moved back to
            // oldLargeRegionList by CleanupMigrationCandidates().
            region->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
            losFullCount++;
            continue;
        }

        // Success: move to fromRegionList and set up ghost route.
        migrationList.DeleteRegion(region);
        fromList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
        region->PrepareForwardableLargeRegion(toAddr);

        ++migratedCount;
        VLOG(REPORT, "LOS migration: region %p @[0x%zx, 0x%zx) size=%zu -> LOS 0x%zx",
             region, region->GetRegionStart(), region->GetRegionEnd(), objSize, toAddr);
    }

    if (migratedCount > 0) {
        // Rebuild ghostFromRegionList to include migration regions.
        // Ghost traversal uses nextRegionIdx0 (saved by PrepareForwardableLargeRegion),
        // forming a valid chain: migration_regions -> small_from_regions.
        fromList.CopyListTo(rm.GetGhostFromRegionList());
    }

    VLOG(REPORT, "LOS migration: %zu scheduled, %zu dead, %zu LOS-full",
         migratedCount, deadCount, losFullCount);
}

void WCollector::CompactLOSIfNeeded()
{
    MRT_PHASE_TIMER("CompactLOSIfNeeded");

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    LOSManager* losMgr = space.GetRegionManager().GetLOSManager();
    if (losMgr == nullptr) {
        return;
    }

    RegionManager& rm = space.GetRegionManager();
    bool hasPendingPlan = losMgr->HasPendingCompactionPlan();
    CompactLOSOnOOMIfNeeded(*losMgr, rm, hasPendingPlan);
}

void WCollector::CompactLOSOnOOMIfNeeded(LOSManager& losMgr, RegionManager& rm, bool hasPendingPlan)
{
    bool needsCompact = (gcReason == GC_REASON_OOM) && (hasPendingPlan || losMgr.NeedsCompaction());
    if (!needsCompact) {
        return;
    }

    // #10A: After CollectLargeGarbage() has reclaimed dead regions, check if LOS
    // now has enough free space to satisfy the allocation that triggered OOM.
    // If so, skip the expensive CompactLOS (STW + memmove + ref scan + rebuild).
    // This avoids frequent compaction in small-heap scenarios where normal GC
    // can free enough space.
    if (!hasPendingPlan && losMgr.CanAllocLargeAfterGC()) {
        DLOG(REPORT, "Skip CompactLOS: GC freed enough LOS space for allocation");
        return;
    }

    ScopedStopTheWorld stw("OOM LOS compact", true, GCPhase::GC_PHASE_POST_TRACE);
    losMgr.SetCompacting(true);
    losMgr.FreezeForCompaction();

    if (hasPendingPlan) {
        losMgr.ClearPendingCompactionPlan();
    }
    size_t freedBytes = losMgr.CompactLOS();
    DLOG(REPORT, "OOM LOS compact done in STW: freed=%zu", freedBytes);

    // After memmove, update all heap references that point to old LOS addresses.
    // This must complete before STW ends, otherwise mutators would use stale pointers.
    UpdateCompactedLOSReferences(losMgr, rm);

    losMgr.ClearMigrationMap();
    losMgr.UnfreezeAfterCompaction();
    losMgr.SetCompacting(false);
    losMgr.SetCompactionRequested(false);
}

void WCollector::UpdateCompactedLOSReferences(LOSManager& losMgr, RegionManager& rm)
{
    const auto& migMap = losMgr.GetMigrationMap();
    if (migMap.empty()) {
        return;
    }

    size_t updatedFieldCount = 0;
    size_t updatedRootCount = 0;
    size_t scannedObjectCount = 0;
    size_t tagStrippedCount = 0;
    size_t rangeSkippedCount = 0;

    // Pre-compute address range of moved LOS objects for fast rejection.
    // Most heap references point to MAIN space, far outside this range,
    // so the range check avoids >99% of hash lookups in migMap.
    uintptr_t minOldAddr = UINTPTR_MAX;
    uintptr_t maxOldAddr = 0;
    for (const auto& entry : migMap) {
        if (entry.first < minOldAddr) {
            minOldAddr = entry.first;
        }
        if (entry.first > maxOldAddr) {
            maxOldAddr = entry.first;
        }
    }

    // --- Helper: update a RefField if it points to a moved LOS object ---
    auto updateRefField = [&](RefField<>& field) {
        BaseObject* target = field.GetTargetObject();
        if (target == nullptr) {
            return;
        }
        uintptr_t addr = reinterpret_cast<uintptr_t>(target);
        // Fast range check: skip hash lookup if address is outside moved LOS region range
        if (LIKELY(addr < minOldAddr || addr > maxOldAddr)) {
            ++rangeSkippedCount;
            return;
        }
        auto it = migMap.find(addr);
        if (it != migMap.end()) {
            bool wasTagged = field.IsTagged();
            field.SetTargetObject(reinterpret_cast<BaseObject*>(it->second));
            if (wasTagged) {
                ++tagStrippedCount;
                VLOG(REPORT, "LOS compact: tag-stripped field @%p: old=%p(tagged) -> new=%p(untagged)",
                     &field, target, reinterpret_cast<BaseObject*>(it->second));
            }
            ++updatedFieldCount;
        }
    };

    // --- Helper: update a root (ObjectRef) if it points to a moved LOS object ---
    auto updateRoot = [&](ObjectRef& ref) {
        if (ref.object == nullptr) {
            return;
        }
        uintptr_t addr = reinterpret_cast<uintptr_t>(ref.object);
        // Fast range check: skip hash lookup if address is outside moved LOS region range
        if (LIKELY(addr < minOldAddr || addr > maxOldAddr)) {
            return;
        }
        auto it = migMap.find(addr);
        if (it != migMap.end()) {
            ref.object = reinterpret_cast<BaseObject*>(it->second);
            ++updatedRootCount;
        }
    };

    // === A. Scan ALL heap objects (MAIN + LOS) and update their reference fields ===
    // ForEachObjUnsafe iterates [regionHeapStart, inactiveZone) for MAIN
    // plus LOS old/recent region lists. Safe to call in STW without extra lock.
    auto objVisitor = [&updateRefField, &scannedObjectCount](BaseObject* obj) {
        obj->ForEachRefField(updateRefField);
        ++scannedObjectCount;
    };
    rm.ForEachObjUnsafe(objVisitor);

    // === B. Scan ALL GC roots ===

    // B1. Static roots (global RefField<> registered via StaticRootTable)
    Heap::GetHeap().VisitStaticRoots(updateRefField);

    // B2. Thread stack roots (mutators are stopped in STW)
    MutatorManager::Instance().VisitAllMutators([&updateRoot](Mutator& mutator) {
        mutator.VisitMutatorRoots(updateRoot);
    });

    // B3. Finalizer roots (finalizer queue + working finalizables)
    collectorResources.GetFinalizerProcessor().VisitRawPointers(updateRoot);

    // B4. Export roots (cross-language exported objects)
    Heap::GetHeap().VisitAllExportRoots(updateRoot);

    // B5. Concurrency model roots (Future, coroutine handles, etc.)
    RootVisitor rootVisitor = updateRoot;
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rootVisitor);

    VLOG(REPORT, "LOS compact ref update: scanned %zu objects, updated %zu fields + %zu roots, "
         "tagStripped %zu, rangeSkipped %zu (migMap range [%p, %p])",
         scannedObjectCount, updatedFieldCount, updatedRootCount, tagStrippedCount,
         rangeSkippedCount, reinterpret_cast<void*>(minOldAddr), reinterpret_cast<void*>(maxOldAddr));
}

void WCollector::Preforward()
{
    ScopedEntryTrace trace("CJRT_GC_PREFORWARD");
    MRT_PHASE_TIMER("Preforward");
    {
        ScopedLightSync scopedLightSync("Preforward", true, GCPhase::GC_PHASE_PREFORWARD);
    }

    GCThreadPool* threadPool = GetThreadPool();
    MRT_ASSERT(threadPool != nullptr, "thread pool is null");
    // forward and fix cj future objects
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardConcurrencyModelRoots(); }));

    // forward and fix finalizer roots.
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardFinalizerProcessorRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllExportFromRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardDiscoveredExternObjects(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllResurrectExportFromObjects(); }));
    threadPool->Start();
    threadPool->WaitFinish();
}


extern "C" void CJ_MRT_RolveCycleRef();
extern "C" void ResolveCycleRefStub(CrossRefHandler, BaseObject*, BaseObject*, void**);

class CJFunc : public BaseObject {
public:
    CrossRefHandler GetHandler()
    {
        return handler;
    }
private:
    CrossRefHandler handler = nullptr;
};

class CJInteropContext : public BaseObject {
public:
    CJFunc* GetCJFunc()
    {
        return static_cast<CJFunc*>(Heap::GetBarrier().ReadReference(this,
            *reinterpret_cast<RefField<false>*>(&cjFunc)));
    }
private:
    CJFunc* cjFunc = nullptr;
};

class CJForeignProxy : public BaseObject {
public:
    CJInteropContext* GetCJInteropContext()
    {
        return static_cast<CJInteropContext*>(Heap::GetBarrier().ReadReference(this,
            *reinterpret_cast<RefField<false>*>(&interopContext)));
    }
private:
    CJInteropContext* interopContext = nullptr;
};

CrossRefHandler WCollector::GetCrossRefHandler(BaseObject *foreignProxy)
{
    return static_cast<CJForeignProxy*>(foreignProxy)->GetCJInteropContext()->GetCJFunc()->GetHandler();
}

void WCollector::ResolveCycleRef()
{
#if defined (__OHOS__)
    size_t i = 0;
    if (!cycleWorkStackMtx.try_lock()) {
        CJ_MRT_RolveCycleRef();
        return;
    }
    for (auto it = cycleRefWorkStack.begin(); it != cycleRefWorkStack.end(); i++) {
        ScopedObjectAccess soa;
        auto phase = GetGCPhase();
        static constexpr size_t taskNum = 100;
        if (phase == GC_PHASE_PREFORWARD || i >= taskNum) {
            cycleWorkStackMtx.unlock();
            CJ_MRT_RolveCycleRef();
            return;
        }
        BaseObject* exportObj = it->first;
        auto& heap = Heap::GetHeap();
        auto id = static_cast<ExportObject*>(exportObj)->GetId();
        if (!heap.CheckExportObjState(id, exportObj)) {
            it = cycleRefWorkStack.erase(it);
            continue;
        }
        if (resurrectedExportObjectes.find(exportObj) != resurrectedExportObjectes.end() ||
            resurrectedExportObjectesForwardPhase.find(exportObj) != resurrectedExportObjectesForwardPhase.end()) {
            it = cycleRefWorkStack.erase(it);
            continue;
        }
        auto externObjs = it->second;
        void* returnUnit = nullptr;
        for (auto externObj : externObjs) {
            auto resolveHook = GetCrossRefHandler(externObj);
            ResolveCycleRefStub(resolveHook, exportObj, externObj, &returnUnit);
        }
        heap.SetExportObjActiveState(id, false);
        it++;
    }
    cycleWorkStackMtx.unlock();
    resurrectedExportObjectes.clear();
    resurrectedExportObjectesForwardPhase.clear();
#endif
}
void WCollector::PostResolveCycleTask()
{
#if defined (__OHOS__)
    if (cycleRefWorkStack.empty()) {
        return;
    }
    CJ_MRT_RolveCycleRef();
#endif
}
void WCollector::DoGarbageCollection()
{
    uint32_t cycleNum = g_gcCycleNum.fetch_add(1);
    VLOG(REPORT, "=== GC START cycle=%u === tagID=%u reason=%d", cycleNum, (unsigned)currentTagID, gcReason);

    TraceHeap();
    PostTrace();

    Preforward();

    ForwardFromSpace();
    FinalizeLOSCompactionIfNeeded();

    // P3: ShrinkInactiveZone — every GC, gated by MAIN fragmentation ratio.
    // Uses tree pruning (RemoveAboveIndex) instead of ClearTrees+rebuild,
    // which avoids the metadata consistency crash with borrow-on-demand.
    // Gate: only enter STW + shrink when MAIN fragmentation > 30%.
    // The reclaimable >= 10% RESERVE check is done inside ShrinkInactiveZone
    // (the live-scan determines actual reclaimable amount; early-exit if nothing).
    {
        RegionManager& rm = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
        double fragRatio = rm.GetMainFragmentationRatio();
        if (fragRatio > 0.30) {
            ScopedStopTheWorld stw("shrink inactive", false);
            size_t reclaimed = rm.ShrinkInactiveZone();
            if (reclaimed > 0) {
                VLOG(REPORT, "WCollector: ShrinkInactiveZone reclaimed %zu bytes (frag=%.1f%%)",
                     reclaimed, fragRatio * 100.0);
            }
        }
    }

    // CompactLOS — every GC, gated by LOS fragmentation ratio > 30%.
    // Moved from PostTrace (OOM-only) to post-GC phase for consistent reclamation.
    // RESERVE borrowed space is returned fully to the watermark (no keep buffer).
    {
        RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
        LOSManager* losMgr = space.GetRegionManager().GetLOSManager();
        if (losMgr != nullptr) {
            double losFragRatio = losMgr->GetFragmentationRatio();
            if (losFragRatio > 0.30) {
                RegionManager& rm = space.GetRegionManager();
                ScopedStopTheWorld stw("LOS compact", true, GCPhase::GC_PHASE_POST_TRACE);
                losMgr->SetCompacting(true);
                losMgr->FreezeForCompaction();
                size_t freedBytes = losMgr->CompactLOS();
                UpdateCompactedLOSReferences(*losMgr, rm);
                losMgr->ClearMigrationMap();
                losMgr->UnfreezeAfterCompaction();
                losMgr->SetCompacting(false);
                losMgr->SetCompactionRequested(false);
                if (freedBytes > 0) {
                    VLOG(REPORT, "WCollector: CompactLOS freed %zu bytes (frag=%.1f%%)",
                         freedBytes, losFragRatio * 100.0);
                }
            }
        }
    }

    // Return unmigrated M3 candidates (UNMOVABLE_FROM_REGION) to oldLargeRegionList.
    // These are regions that could not be migrated because LOS + RESERVE were both
    // exhausted in BuildLargeObjectMigrationRoutes.
    reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager().CleanupMigrationCandidates();

    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    MergeResurrectExportObjects();
    PostResolveCycleTask();
    FlipTagID();
    ForwardDataManager::GetForwardDataManager().SetTagID(currentTagID);

    CollectSmallSpace();
    ForwardDataManager::GetForwardDataManager().UnbindPreviousLiveInfo();

    // Notify RESERVE manager of GC cycle (for cooldown tracking).
    {
        RegionManager& rm = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
        ReserveManager* resMgr = rm.GetReserveManager();
        if (resMgr != nullptr) {
            resMgr->NotifyGCCycle();
        }
    }

    reinterpret_cast<RegionSpace&>(theAllocator).DumpRegionStats("region statistics when gc ends");
}

void WCollector::FinalizeLOSCompactionIfNeeded()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& rm = space.GetRegionManager();
    LOSManager* losMgr = space.GetRegionManager().GetLOSManager();
    if (losMgr == nullptr) {
        return;
    }

    (void)rm;

    if (!losMgr->IsCompactionFrozen() && !losMgr->IsCompacting()) {
        return;
    }

    if (losMgr->HasPendingCompactionPlan()) {
        size_t finalized = losMgr->FinalizePendingCompaction();
        DLOG(REPORT, "LOS finalize: finalized=%zu, remaining=%zu",
             finalized, losMgr->GetPendingCompactionPlanSize());
    }

    if (losMgr->HasPendingCompactionPlan()) {
        // Keep compaction window open until all pending entries converge.
        return;
    }

    losMgr->UnfreezeAfterCompaction();
    losMgr->SetCompacting(false);
    losMgr->SetCompactionRequested(false);
}

void WCollector::MarkNewObject(BaseObject* obj)
{
    GCPhase mutatorPhase = Mutator::GetMutator()->GetMutatorPhase();
    if (UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_ENUM) || UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_TRACE) ||
        UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER)) {
        MarkObject(obj);
    }
}

void WCollector::ProcessFinalizers()
{
    std::function<bool(BaseObject*)> finalizable = [this](BaseObject* obj) { return !IsMarkedObject(obj); };
    FinalizerProcessor& fp = collectorResources.GetFinalizerProcessor();
    fp.EnqueueFinalizables(finalizable, snapshotFinalizerNum);
    fp.Notify();
}

BaseObject* WCollector::ForwardObject(BaseObject* obj)
{
    BaseObject* to = TryForwardObject(obj);
    return (to != nullptr) ? to : obj;
}

BaseObject* WCollector::TryForwardObject(BaseObject* obj)
{
    CHECK_DETAIL(Heap::IsHeapAddress(obj), "TryForwardObject got non-heap pointer %p", obj);

    // A mutator barrier can race with the GC phase transition.  If we are no
    // longer in a forward-capable phase, the GC thread has already completed
    // ForwardFromSpace and all from-objects are forwarded.  Instead of calling
    // ForwardObjectImpl (which CHECK-aborts outside the forward phase), resolve
    // any already-forwarded object via FindToVersion.  If the object wasn't in
    // a ghost-from region (i.e., wasn't forwarded), FindToVersion returns nullptr
    // and ForwardObject falls back to the original object — which is correct
    // since there is nothing to forward.
    GCPhase phase = GetGCPhase();
    if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
        return FindToVersion(obj);
    }

    RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr) {
            RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
            LOSManager* losMgr = space.GetRegionManager().GetLOSManager();
            if (losMgr != nullptr && losMgr->IsCompacting() && losMgr->HasPendingCompactionPlan()) {
                if (losMgr->EnsureCompactionRouteForObject(reinterpret_cast<uintptr_t>(obj))) {
                    region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
                }
            }
        }

        if (region == nullptr) {
        return nullptr;
    }

    if (fwdTable.RouteRegion(region)) {
        if (region->TryLockReadFromRegion()) {
            BaseObject* toVersion = ForwardObjectImpl(obj, region);
            region->UnlockReadFromRegion();
            return toVersion;
        } else {
            return FindToVersion(obj);
        }
    } else if (region->IsCompacted()) {
        return FindToVersion(obj);
    }
    return nullptr;
}

BaseObject* WCollector::ForwardObjectImpl(BaseObject* obj, RegionInfo* ghostFromRegion)
{
    CHECK(GetGCPhase() == GCPhase::GC_PHASE_PREFORWARD || GetGCPhase() == GCPhase::GC_PHASE_FORWARD);
    do {
        StateWord oldWord = obj->GetStateWord();

        // 1. object has already been forwarded
        if (obj->IsForwarded()) {
            auto toObj = GetForwardPointer(obj, ghostFromRegion);
            if (toObj == nullptr || !Heap::IsHeapAddress(toObj)) {
                LOG(RTLOG_ERROR, "invalid forwarding target for obj %p in ghost region %p: to=%p",
                    obj, ghostFromRegion, toObj);
                return nullptr;
            }
            DLOG(FORWARD, "skip forwarded obj %p -> %p<%p>", obj, toObj, toObj->GetTypeInfo());
            return toObj;
        }

        // 2. object is being forwarded, spin until it is forwarded (or gets its own forwarded address)
        if (oldWord.IsLockedWord()) {
            sched_yield();
            continue;
        }

        // 3. hope we can forward this object
        if (obj->TryLockObject(oldWord)) {
            return ForwardObjectExclusive(obj);
        }
    } while (true);
    LOG(RTLOG_FATAL, "forwardObject exit in wrong path");
    return nullptr;
}

BaseObject* WCollector::ForwardObjectExclusive(BaseObject* obj)
{
    size_t size = RegionSpace::GetAllocSize(*obj);
    BaseObject* toObj = fwdTable.RouteObject(obj);
    CHECK_DETAIL(toObj != nullptr, "invalid object route");
    DLOG(FORWARD, "forward obj %p<%p>(%zu) to %p", obj, obj->GetTypeInfo(), size, toObj);

    // --- Pre-copy diagnostic: verify from-version has valid typeInfo ---
    TypeInfo* fromTypeInfo = obj->GetTypeInfo();
    if (UNLIKELY(fromTypeInfo == nullptr)) {
        LOG(RTLOG_ERROR, "ForwardObjectExclusive: from obj %p has null typeInfo before copy! size=%zu toObj=%p",
            obj, size, toObj);
    }

    CopyObject(*obj, *toObj, size);

    // --- Post-copy diagnostic: verify to-version has valid data ---
    size_t* toRaw = reinterpret_cast<size_t*>(toObj);
    if (UNLIKELY(toRaw[0] == 0 && toRaw[1] == 0)) {
        size_t* fromRaw = reinterpret_cast<size_t*>(obj);
        RegionInfo* fromRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        RegionInfo* toRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(toObj));
        LOG(RTLOG_ERROR, "ForwardObjectExclusive: COPY FAILED! from=%p to=%p size=%zu "
            "fromRaw=[%#zx,%#zx] toRaw=[%#zx,%#zx] "
            "fromRegion=%p type=%u [%#zx,%#zx) toRegion=%p type=%u [%#zx,%#zx) allocPtr=%#zx",
            obj, toObj, size,
            fromRaw[0], fromRaw[1], toRaw[0], toRaw[1],
            fromRegion, fromRegion->GetRegionType(), fromRegion->GetRegionStart(), fromRegion->GetRegionEnd(),
            toRegion, toRegion->GetRegionType(), toRegion->GetRegionStart(), toRegion->GetRegionEnd(),
            toRegion->GetRegionAllocPtr());
    }

    toObj->SetStateCode(ObjectState::NORMAL);
    std::atomic_thread_fence(std::memory_order_release);
    obj->UnlockObject(ObjectState::FORWARDED);
    return toObj;
}

void WCollector::CollectSmallSpace()
{
    GCStats& stats = GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    {
        MRT_PHASE_TIMER("CollectFromSpaceGarbage");
        stats.collectedBytes += stats.smallGarbageSize;
        space.CollectFromSpaceGarbage();
    }

    size_t candidateBytes = stats.fromSpaceSize + stats.pinnedSpaceSize + stats.largeSpaceSize;
    stats.garbageRatio = (candidateBytes > 0) ? static_cast<float>(stats.collectedBytes) / candidateBytes : 0;

    stats.liveBytesAfterGC = space.AllocatedBytes();

    VLOG(REPORT,
         "collect %zu B: old small %zu - %zu B, old pinned %zu - %zu B, old large %zu - %zu B. garbage ratio %.2f%%",
         stats.collectedBytes, stats.fromSpaceSize, stats.smallGarbageSize, stats.pinnedSpaceSize,
         stats.pinnedGarbageSize, stats.largeSpaceSize, stats.largeGarbageSize,
         stats.garbageRatio * 100); // The base of the percentage is 100

    VLOG(REPORT, "start to release heap garbage memory");
#if defined(__EULER__)
    Heap::GetHeap().GetAllocator().TryReclaimGarbageMemory();
#endif
    collectorResources.GetFinalizerProcessor().NotifyToReclaimGarbage();
}

bool WCollector::ShouldIgnoreRequest(GCRequest& request) { return request.ShouldBeIgnored(); }
} // namespace MapleRuntime
