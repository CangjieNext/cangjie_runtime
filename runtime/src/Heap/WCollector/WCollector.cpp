// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "WCollector.h"

#include "Concurrency/Concurrency.h"
#include "Heap/Allocator/LOSManager.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
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
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by object %p: %s and offset %zd", latest,
                 obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
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
    CompactLOSIfNeeded();
    CollectPinnedGarbage();
    RefineFromSpace();
    fwdTable.PrepareForwardTable();
}

void WCollector::CompactLOSIfNeeded()
{
    MRT_PHASE_TIMER("CompactLOSIfNeeded");

    // Safety gate: keep compaction execution opt-in until non-STW path is fully stabilized.
    const char* compactEnabled = std::getenv("cjEnableLOSCompact");
    if (compactEnabled == nullptr || compactEnabled[0] != '1') {
        return;
    }

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    LOSManager* losMgr = space.GetRegionManager().GetLOSManager();
    if (losMgr == nullptr) {
        return;
    }

    RegionManager& rm = space.GetRegionManager();
    std::vector<uintptr_t> fallbackObjs = rm.ConsumeLOSFallbackLargeObjects();
    losMgr->AddPostGcMigrationCandidates(fallbackObjs);

    bool hasPendingPlan = losMgr->HasPendingCompactionPlan();
    CompactLOSOnOOMIfNeeded(*losMgr, rm, hasPendingPlan);
    InstallLOSMigrationRoutesIfNeeded(*losMgr, rm);
}

void WCollector::CompactLOSOnOOMIfNeeded(LOSManager& losMgr, RegionManager& rm, bool hasPendingPlan)
{
    (void)rm;
    bool needsCompact = (gcReason == GC_REASON_OOM) && (hasPendingPlan || losMgr.NeedsCompaction());
    if (!needsCompact) {
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

    losMgr.UnfreezeAfterCompaction();
    losMgr.SetCompacting(false);
    losMgr.SetCompactionRequested(false);
}

void WCollector::InstallLOSMigrationRoutesIfNeeded(LOSManager& losMgr, RegionManager& rm)
{
    // Safety gate for MAIN large-object route migration.
    // Keep disabled by default until metadata/list interaction is fully stabilized.
    const char* routeEnabled = std::getenv("cjEnableLOSRouteMigration");
    if (routeEnabled == nullptr || routeEnabled[0] != '1') {
        return;
    }

    // Route installation is currently limited to OOM cycle for stability.
    if (gcReason != GC_REASON_OOM) {
        return;
    }

    // No dedicated background migration thread in Phase 1:
    // Install route metadata in a short STW window for list/metadata safety,
    // then rely on barrier + next-GC force-forward convergence for concurrent move/access.
    size_t installed = 0;
    {
        ScopedStopTheWorld stw("Install LOS migration routes", true, GCPhase::GC_PHASE_POST_TRACE);
        installed = losMgr.InstallPostGcMainMigrationRoutes(
            rm.GetOldLargeRegionList(), rm.GetGarbageRegionList());
    }
    if (installed > 0 || losMgr.HasPendingPostGcMainMigrations()) {
        DLOG(REPORT, "LOS post-gc migration: installed=%zu pending=%zu",
             installed, losMgr.GetPendingPostGcMainMigrationCount());
    }
}

void WCollector::MigrateLargeObjectsToLOS()
{
    MRT_PHASE_TIMER("MigrateLargeObjectsToLOS");
    // DISABLED: MigrateLargeObjectsFromMain copies objects from MAIN to LOS and
    // UpdateMigratedReferences scans the entire heap to update references.
    // Without STW, mutators concurrently read stale references → SIGSEGV.
    // TODO: Re-enable with ScopedStopTheWorld.
    return;
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    LOSManager* losMgr = space.GetRegionManager().GetLOSManager();
    if (losMgr == nullptr) {
        return;
    }
    RegionManager& rm = space.GetRegionManager();
    // MigrateLargeObjectsFromMain appends to migrationMap (may contain compaction entries).
    size_t migratedCount = losMgr->MigrateLargeObjectsFromMain(
        rm.GetOldLargeRegionList(), rm.GetGarbageRegionList());
    (void)migratedCount;
    // Single unified reference update: covers both compaction + migration entries.
    // CompactLOS() only builds forwarding entries without scanning; we scan once here.
    if (losMgr->HasPendingMigrations()) {
        losMgr->UpdateMigratedReferences();
    }
    // Clear all forwarding entries (compaction + migration) after everything is done.
    losMgr->ClearMigrationState();
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
    TraceHeap();
    PostTrace();

    Preforward();

    ForwardFromSpace();
    ForceForwardPendingLOSObjects();
    FinalizeLOSCompactionIfNeeded();

    // M4b: ShrinkInactiveZone is DISABLED.
    // The WCollector is a concurrent collector (no STW — ScopedStopTheWorld is commented out
    // in CopyCollector::RunGarbageCollection). Mutators run concurrently during the entire
    // GC cycle. ShrinkInactiveZone does ClearTrees + re-scan, which races with mutator's
    // TakeRegion → ReclaimRegion (InitFreeUnits then AddGarbageUnits). A mutator can call
    // InitFreeUnits (marking region as free) before AddGarbageUnits; concurrently the
    // re-scan sees the region as free and also adds it → double-insert into CartesianTree.
    // TODO(M4b): Re-enable with ScopedStopTheWorld if shrinking is needed.
    // reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager().ShrinkInactiveZone();

    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    MergeResurrectExportObjects();
    PostResolveCycleTask();
    FlipTagID();
    ForwardDataManager::GetForwardDataManager().SetTagID(currentTagID);

    CollectSmallSpace();
    ForwardDataManager::GetForwardDataManager().UnbindPreviousLiveInfo();

    // M5: Return RESERVE-borrowed space after GC.
    // MAIN: dead borrowed regions (intercepted in ReclaimRegion/ReleaseRegion) are returned.
    // LOS: dead borrowed regions (intercepted in ReclaimLOSRegion/ReleaseLOSRegion) are returned.
    // pendingReserveReturns is protected by pendingReserveReturnsMutex.
    {
        RegionManager& rm = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
        rm.ReturnBorrowedToReserve();
        LOSManager* losMgr = rm.GetLOSManager();
        if (losMgr != nullptr) {
            losMgr->ReturnBorrowedToReserve();
        }
        ReserveManager* resMgr = rm.GetReserveManager();
        if (resMgr != nullptr) {
            resMgr->NotifyGCCycle();
        }
    }

    reinterpret_cast<RegionSpace&>(theAllocator).DumpRegionStats("region statistics when gc ends");
}

void WCollector::ForceForwardPendingLOSObjects()
{
    // GC主动收敛：ReadBarrier是按访问触发，不能保证覆盖所有LOS对象。
    // 若存在待迁移/待压缩的forwarding条目，GC线程在Forward阶段后主动遍历一次，
    // 确保未被mutator触达的对象也完成forward，后续才能安全回收旧region并重建树。
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    LOSManager* losMgr = space.GetRegionManager().GetLOSManager();
    if (losMgr == nullptr) {
        return;
    }

    constexpr size_t MAX_CONVERGE_ROUNDS = 4;
    size_t totalForcedCount = 0;

    for (size_t round = 0; round < MAX_CONVERGE_ROUNDS; ++round) {
        std::vector<uintptr_t> oldObjAddrs;
        if (losMgr->HasPendingMigrations()) {
            oldObjAddrs = losMgr->GetPendingMigrationSourceAddresses();
        }
        if (losMgr->HasPendingCompactionPlan()) {
            auto plan = losMgr->GetPendingCompactionPlanSnapshot();
            oldObjAddrs.reserve(oldObjAddrs.size() + plan.size());
            for (const auto& entry : plan) {
                RegionInfo* oldRegion = RegionInfo::GetRegionInfoAt(entry.oldStart);
                if (oldRegion == nullptr || !oldRegion->IsGhostFromRegion()) {
                    continue;
                }
                oldObjAddrs.push_back(entry.oldStart + RegionSpace::HEADER_SIZE);
            }
        }
        if (oldObjAddrs.empty()) {
            break;
        }

        size_t forcedCount = 0;
        for (uintptr_t addr : oldObjAddrs) {
            if (!Heap::IsHeapAddress(reinterpret_cast<BaseObject*>(addr))) {
                continue;
            }
            BaseObject* oldObj = reinterpret_cast<BaseObject*>(addr);
            BaseObject* latest = TryForwardObject(oldObj);
            if (latest != nullptr) {
                ++forcedCount;
            }
        }

        totalForcedCount += forcedCount;
        if (forcedCount == 0) {
            break;
        }
    }

    DLOG(REPORT, "LOS force-forward pending objects done: forced=%zu", totalForcedCount);
}

void WCollector::FinalizeLOSCompactionIfNeeded()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& rm = space.GetRegionManager();
    LOSManager* losMgr = space.GetRegionManager().GetLOSManager();
    if (losMgr == nullptr) {
        return;
    }

    if (losMgr->HasPendingPostGcMainMigrations()) {
        size_t reclaimed = 0;
        {
            ScopedStopTheWorld stw("Finalize LOS migration routes", true, GCPhase::GC_PHASE_FORWARD);
            reclaimed = losMgr->FinalizePostGcMainMigrations(
                rm.GetOldLargeRegionList(), rm.GetGarbageRegionList());
        }
        DLOG(REPORT, "LOS post-gc migration finalize: reclaimed=%zu pending=%zu",
             reclaimed, losMgr->GetPendingPostGcMainMigrationCount());
    }

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
