// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Collector/FinalizerWorkList.h"

#include "Common/ScopedObjectAccess.h"
#include "ExceptionManager.inline.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MObject.h"

namespace MapleRuntime {
U32 FinalizerWorkList::VisitFinalizers(const RootVisitor& visitor)
{
    U32 count = 0;
    std::lock_guard<std::mutex> l(listLock);
    for (BaseObject*& obj : finalizers) {
        visitor(reinterpret_cast<ObjectRef&>(obj));
        ++count;
    }
    return count;
}

void FinalizerWorkList::VisitGCRoots(const RootVisitor& visitor)
{
    std::lock_guard<std::mutex> l(listLock);
    for (BaseObject*& obj : finalizables) {
        visitor(reinterpret_cast<ObjectRef&>(obj));
    }
    for (BaseObject*& obj : workingFinalizables) {
        visitor(reinterpret_cast<ObjectRef&>(obj));
    }
}

void FinalizerWorkList::VisitRawPointers(const RootVisitor& visitor)
{
    std::lock_guard<std::mutex> l(listLock);
    for (BaseObject*& obj : finalizables) {
        visitor(reinterpret_cast<ObjectRef&>(obj));
    }
    for (BaseObject*& obj : workingFinalizables) {
        visitor(reinterpret_cast<ObjectRef&>(obj));
    }
    for (BaseObject*& obj : finalizers) {
        visitor(reinterpret_cast<ObjectRef&>(obj));
    }
}

void FinalizerWorkList::EnqueueFinalizables(const std::function<bool(BaseObject*)>& finalizable, U32 countLimit)
{
    std::lock_guard<std::mutex> l(listLock);
    auto it = finalizers.begin();
    while (it != finalizers.end() && countLimit != 0) {
        RefField<> tmpField(reinterpret_cast<MAddress>(*it));
        BaseObject* obj = tmpField.GetTargetObject();
        --countLimit;
        if (finalizable(obj)) {
            finalizables.push_back(reinterpret_cast<BaseObject*>(tmpField.GetFieldValue()));
            it = finalizers.erase(it);
        } else {
            ++it;
        }
    }

    if (!finalizables.empty()) {
        hasFinalizableJob.store(true, std::memory_order_relaxed);
    }
}

void FinalizerWorkList::ProcessFinalizableList(const std::function<bool()>& shouldKeepRunning)
{
    auto itor = workingFinalizables.begin();
    while (itor != workingFinalizables.end() && shouldKeepRunning()) {
        ScopedObjectAccess soa;
        CHECK_DETAIL(ExceptionManager::GetPendingException() == nullptr, "should not exist pending exception");
        RefField<> tmpField(reinterpret_cast<MAddress>(*itor));
        BaseObject* finalizeObjAddr = Heap::GetHeap().GetBarrier().ReadStaticRef(tmpField);

        TypeInfo* classInfo = reinterpret_cast<MObject*>(finalizeObjAddr)->GetTypeInfo();
        FuncRef finalizerMethod = classInfo->GetFinalizeMethod();
        Mutator* mutator = ThreadLocal::GetMutator();

        CHECK_DETAIL(finalizerMethod != nullptr, "%p has no finalize method", finalizeObjAddr);
        void (*finalizer)(BaseObject*, TypeInfo*) = reinterpret_cast<void (*)(BaseObject*, TypeInfo*)>(finalizerMethod);
        mutator->SetManagedContext(true);
        DLOG(FINALIZE, "tid %u finalize object %p", mutator->GetTid(), finalizeObjAddr);
        uintptr_t threadData = MapleRuntime::MRT_GetThreadLocalData();
        ExecuteCangjieStub(finalizeObjAddr, finalizeObjAddr->GetTypeInfo(), 0, reinterpret_cast<void*>(finalizer),
                           reinterpret_cast<void*>(threadData), 0);
        mutator->SetManagedContext(false);

        if (ExceptionManager::HasFatalException()) {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
            ExceptionManager::DumpException();
#endif
            LOG(RTLOG_FATAL, "FatalException happened in finalizer");
        }
        ExceptionManager::ClearPendingException();
#ifdef MRT_USE_MARK_SWEEP
        // A Finalizer that started before MSGC may finish after the initial
        // root snapshot. Shade it before removing the working-list root so a
        // self-resurrection (or its current strong closure) cannot be swept.
        // MarkNewObject is phase-gated and defers ref-field scanning to the
        // MarkTermination fixed point.
        Heap::GetHeap().GetCollector().MarkNewObject(finalizeObjAddr);
#endif
        {
            std::lock_guard<std::mutex> l(listLock);
            itor = workingFinalizables.erase(itor);
        }
    }
}

void FinalizerWorkList::ProcessFinalizables(const std::function<bool()>& shouldKeepRunning)
{
    MRT_PHASE_TIMER("Finalizer", FINALIZE);
    {
        ScopedObjectAccess soa;
        std::lock_guard<std::mutex> l(listLock);
        if (workingFinalizables.empty()) {
            workingFinalizables.swap(finalizables);
        }
    }
    DLOG(FINALIZE, "finalizer: working size %zu", workingFinalizables.size());
    ProcessFinalizableList(shouldKeepRunning);
    {
        std::lock_guard<std::mutex> l(listLock);
        bool hasPending = !finalizables.empty() || !workingFinalizables.empty();
        hasFinalizableJob.store(hasPending, std::memory_order_relaxed);
    }
}

void FinalizerWorkList::RegisterFinalizer(BaseObject* obj)
{
    RefField<> tmpField(nullptr);
    Heap::GetHeap().GetBarrier().WriteStaticRef(tmpField, obj);
    std::lock_guard<std::mutex> l(listLock);
    finalizers.push_back(reinterpret_cast<BaseObject*>(tmpField.GetFieldValue()));
}
} // namespace MapleRuntime
