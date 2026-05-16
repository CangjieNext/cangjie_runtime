// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "WCollector.h"

#include <cstdio>
#include <time.h>

#include "Concurrency/Concurrency.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
static unsigned long long g_houdini_trace_heap_seq = 0;

static inline void WCollectorConsoleProbe(const char* msg)
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    std::printf("[%lld.%06ld] %s\n", static_cast<long long>(ts.tv_sec), ts.tv_nsec / 1000, msg);
    std::fflush(stdout);
}

static inline void WCollectorMutatorSnapshotProbe(const char* msg)
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    MutatorManager& mm = MutatorManager::Instance();
    std::printf("[%lld.%06ld] %s gcPhase=%d worldStopped=%d syncTriggered=%d mutatorCount=%zu\n",
        static_cast<long long>(ts.tv_sec), ts.tv_nsec / 1000, msg, static_cast<int>(Heap::GetHeap().GetGCPhase()),
        static_cast<int>(mm.WorldStopped()), static_cast<int>(mm.SyncTriggered()), mm.GetMutatorCount());
    MutatorManager::Instance().VisitAllMutators([&](Mutator& mutator) {
        std::printf("[%lld.%06ld] HOUDINI_STATES mutator=%p tid=%llu phase=%d saferegion=%d managed=%d cjthread=%p\n",
            static_cast<long long>(ts.tv_sec), ts.tv_nsec / 1000, &mutator,
            static_cast<unsigned long long>(mutator.GetTid()), static_cast<int>(mutator.GetMutatorPhase()),
            static_cast<int>(mutator.InSaferegion()), static_cast<int>(mutator.IsManagedContext()),
            mutator.GetCjthreadPtr());
    });
    std::fflush(stdout);
}

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
    return regionInfo->IsUnmovableFromRegion();
}

bool WCollector::MarkObject(BaseObject* obj) const
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
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
                DLOG(TRACE, "update obj %p<%p>(%zu)+%zu ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(),
                     obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field, oldRef.GetFieldValue(),
                     tmpField.GetFieldValue());
            } else {
                DLOG(TRACE, "update ref@%p: 0x%zx -> %p", &field, oldRef.GetFieldValue(), toObj);
            }
            return true;
        } else {
            if (obj != nullptr) {
                DLOG(TRACE,
                     "update obj %p<%p>(%zu)+%zu but cas failed ref-field@%p: %#zx(%#zx) -> %#zx but cas failed ", obj,
                     obj->GetTypeInfo(), obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field,
                     oldRef.GetFieldValue(), field.GetFieldValue(), tmpField.GetFieldValue());
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
    CHECK_DETAIL(latest->IsValidObject(), "Enum static root %p(%p) encounters invalid object", latest, &field);
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(), latest,
             latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(ENUM, "enum static ref@%p: %#zx=>%#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(),
             newField.GetFieldValue(), latest, latest->GetTypeInfo(), latest->GetSize());
    } else {
        DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(), latest,
             latest->GetTypeInfo(), latest->GetSize());
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
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by object %p: %s and offset %zd", latest,
                 obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, oldField.GetFieldValue(),
             newField.GetFieldValue(), latest, latest->GetTypeInfo(), latest->GetSize());
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
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, oldField.GetFieldValue(),
            newField.GetFieldValue(), latest, latest->GetTypeInfo(), latest->GetSize());
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
    unsigned long long runId = ++g_houdini_trace_heap_seq;
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    std::printf("[%lld.%06ld] HOUDINI_RUN TraceHeap.run_id=%llu collector=%p phase=%d tag=%u\n",
        static_cast<long long>(ts.tv_sec), ts.tv_nsec / 1000, runId, this, static_cast<int>(GetGCPhase()),
        currentTagID);
    std::fflush(stdout);
    LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.enter collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    WorkStack workStack = NewWorkStack();
    WorkStack foreignStack = NewWorkStack();
    WCollectorConsoleProbe("HOUDINI_RAW TraceHeap.after_NewWorkStack_probe_1");
    WCollectorConsoleProbe("HOUDINI_RAW TraceHeap.after_NewWorkStack_probe_2");
    LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.after_NewWorkStack collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    // assemble garbage candidates for tracing.
    WCollectorConsoleProbe("HOUDINI_RAW TraceHeap.before_AssembleGarbageCandidates_probe_1");
    WCollectorConsoleProbe("HOUDINI_RAW TraceHeap.before_AssembleGarbageCandidates_probe_2");
    LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.before_AssembleGarbageCandidates collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    WCollectorMutatorSnapshotProbe("HOUDINI_SNAPSHOT TraceHeap.before_AssembleGarbageCandidates");
    WCollectorConsoleProbe("HOUDINI_STW TraceHeap.before_PreAssembleSTW");
    {
        ScopedStopTheWorld scopedStopTheWorld("TraceHeap.PreAssembleCandidates", false);
        WCollectorConsoleProbe("HOUDINI_STW TraceHeap.in_PreAssembleSTW");
        WCollectorMutatorSnapshotProbe("HOUDINI_SNAPSHOT TraceHeap.in_PreAssembleSTW");
        reinterpret_cast<RegionSpace&>(theAllocator).AssembleGarbageCandidates();
        WCollectorConsoleProbe("HOUDINI_STW TraceHeap.after_AssembleGarbageCandidates_in_STW");
    }
    WCollectorConsoleProbe("HOUDINI_STW TraceHeap.after_PreAssembleSTW");
    LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.after_AssembleGarbageCandidates collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    WCollectorConsoleProbe("HOUDINI_RAW TraceHeap.after_AssembleGarbageCandidates_probe_1");
    WCollectorConsoleProbe("HOUDINI_RAW TraceHeap.after_AssembleGarbageCandidates_probe_2");

    {
        MRT_PHASE_TIMER("enum roots & update old pointers within");
        WCollectorConsoleProbe("HOUDINI_RAW TraceHeap.before_TransitionToEnum_probe_1");
        WCollectorConsoleProbe("HOUDINI_RAW TraceHeap.before_TransitionToEnum_probe_2");
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.before_TransitionToEnum collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
        TransitionToGCPhase(GCPhase::GC_PHASE_ENUM, true);
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.after_TransitionToEnum collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.before_DoEnumeration collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
        DoEnumeration(workStack, foreignStack);
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.after_DoEnumeration collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
    }

    {
        MRT_PHASE_TIMER("trace live objects & update old pointers in ref-fields");
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.before_ResetMarkedObjectCount collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
        markedObjectCount.store(0, std::memory_order_relaxed);
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.after_ResetMarkedObjectCount collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.before_TransitionToTrace collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.after_TransitionToTrace collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.before_PrepareTrace collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.after_PrepareTrace collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.before_DoTracing collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
        DoTracing(workStack, foreignStack);
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.after_DoTracing collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);

        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.before_ProcessFinalizers collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
        ProcessFinalizers();
        LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.after_ProcessFinalizers collector=%p phase=%d tag=%u", this,
            static_cast<int>(GetGCPhase()), currentTagID);
    }
    LOG(RTLOG_ERROR, "HOUDINI_GC TraceHeap.exit collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
}

void WCollector::PostTrace()
{
    MRT_PHASE_TIMER("PostTrace");
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
    CollectPinnedGarbage();
    RefineFromSpace();
    fwdTable.PrepareForwardTable();
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
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.enter collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_TraceHeap collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    TraceHeap();
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.after_TraceHeap collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_PostTrace collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    PostTrace();
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.after_PostTrace collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);

    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_Preforward collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    Preforward();
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.after_Preforward collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);

    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_ForwardFromSpace collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    ForwardFromSpace();
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.after_ForwardFromSpace collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);

    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_TransitionToIdle collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.after_TransitionToIdle collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_MergeResurrectExportObjects collector=%p phase=%d "
        "tag=%u", this, static_cast<int>(GetGCPhase()), currentTagID);
    MergeResurrectExportObjects();
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.after_MergeResurrectExportObjects collector=%p phase=%d tag=%u",
        this, static_cast<int>(GetGCPhase()), currentTagID);
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_PostResolveCycleTask collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    PostResolveCycleTask();
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.after_PostResolveCycleTask collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_FlipTagID collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    FlipTagID();
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.after_FlipTagID collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_SetTagID collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    ForwardDataManager::GetForwardDataManager().SetTagID(currentTagID);
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.after_SetTagID collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);

    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_CollectSmallSpace collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    CollectSmallSpace();
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.after_CollectSmallSpace collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_UnbindPreviousLiveInfo collector=%p phase=%d tag=%u",
        this, static_cast<int>(GetGCPhase()), currentTagID);
    ForwardDataManager::GetForwardDataManager().UnbindPreviousLiveInfo();
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.after_UnbindPreviousLiveInfo collector=%p phase=%d tag=%u",
        this, static_cast<int>(GetGCPhase()), currentTagID);

    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.before_DumpRegionStats collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
    reinterpret_cast<RegionSpace&>(theAllocator).DumpRegionStats("region statistics when gc ends");
    LOG(RTLOG_ERROR, "HOUDINI_GC DoGarbageCollection.exit collector=%p phase=%d tag=%u", this,
        static_cast<int>(GetGCPhase()), currentTagID);
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
    RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
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
            DLOG(FORWARD, "skip forwarded obj %p -> %p<%p>(%zu)", obj, toObj, toObj->GetTypeInfo(), toObj->GetSize());
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
    CopyObject(*obj, *toObj, size);
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
