// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_FINALIZER_WORK_LIST_H
#define MRT_FINALIZER_WORK_LIST_H

#include <atomic>
#include <climits>
#include <functional>
#include <list>
#include <mutex>

#include "Base/Panic.h"
#include "Common/PageAllocator.h"
#include "Common/TypeDef.h"
#include "Heap/Collector/Collector.h"

namespace MapleRuntime {
#ifdef __APPLE__
template<typename T>
using ManagedFinalizerList = std::list<T>;
#else
template<typename T>
using ManagedFinalizerList = std::list<T, StdContainerAllocator<T, FINALIZER_PROCESSOR>>;
#endif

class FinalizerWorkList {
public:
    FinalizerWorkList() = default;
    ~FinalizerWorkList() = default;

    U32 VisitFinalizers(const RootVisitor& visitor);
    void VisitGCRoots(const RootVisitor& visitor);
    void VisitRawPointers(const RootVisitor& visitor);

    void EnqueueFinalizables(const std::function<bool(BaseObject*)>& finalizable, U32 countLimit = UINT_MAX);
    void RegisterFinalizer(BaseObject* obj);
    void ProcessFinalizables(const std::function<bool()>& shouldKeepRunning);
    bool HasFinalizableJob() const { return hasFinalizableJob.load(std::memory_order_relaxed); }

private:
    void ProcessFinalizableList(const std::function<bool()>& shouldKeepRunning);

    std::mutex listLock;
    ManagedFinalizerList<BaseObject*> finalizers;
    ManagedFinalizerList<BaseObject*> finalizables;
    ManagedFinalizerList<BaseObject*> workingFinalizables;
    std::atomic<bool> hasFinalizableJob = { false };
};
} // namespace MapleRuntime

#endif // MRT_FINALIZER_WORK_LIST_H