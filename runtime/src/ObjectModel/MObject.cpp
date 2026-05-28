// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Base/Log.h"
#include "MObject.inline.h"
#include "Inspector/CjAllocData.h"
#include "Heap/Heap.h"
#ifdef MRT_USE_MARK_SWEEP
#include "Mutator/ThreadLocal.h"
#endif
namespace MapleRuntime {
MObject* MObject::NewObject(TypeInfo* ti, MSize size, AllocType allocType)
{
#ifdef MRT_USE_MARK_SWEEP
    ThreadLocalData* local = ThreadLocal::GetThreadLocalData();
    auto addr = HeapManager::Allocate(size, local, allocType);
#else
    auto addr = HeapManager::Allocate(size, allocType);
#endif
    if (LIKELY(addr != NULL_ADDRESS)) {
        // Debug: validate allocated address is in heap range
        if (UNLIKELY(!Heap::IsHeapAddress(addr))) {
            LOG(RTLOG_FATAL, "[MS-GC] NewObject: Allocate returned non-heap addr=0x%zx, "
                "size=%u, allocType=%d, ti=%p",
                addr, size, (int)allocType, (void*)ti);
        }
        (void)SetClassInfo(addr, ti);
// #ifdef MRT_USE_MARK_SWEEP
//         Heap::GetHeap().GetCollector().MarkNewObject(reinterpret_cast<BaseObject*>(addr));
// #endif
    } else {
        return nullptr;
    }
#if defined(__OHOS__) && (__OHOS__ == 1)
    if (CjAllocData::GetCjAllocData()->IsRecording()) {
        CjAllocData::GetCjAllocData()->RecordAllocNodes(ti, size);
    }
#endif
    return Cast<MObject>(addr);
}

MObject* MObject::NewPinnedObject(TypeInfo* ti, MSize size)
{
    CHECK_DETAIL(ti->IsObjectType() == true, "must be object class.");
#ifdef MRT_USE_MARK_SWEEP
    ThreadLocalData* local = ThreadLocal::GetThreadLocalData();
    auto addr = HeapManager::Allocate(size, local, AllocType::PINNED_OBJECT);
#else
    auto addr = HeapManager::Allocate(size, AllocType::PINNED_OBJECT);
#endif
    if (LIKELY(addr != NULL_ADDRESS)) {
        (void)SetClassInfo(addr, ti);
// #ifdef MRT_USE_MARK_SWEEP
//         Heap::GetHeap().GetCollector().MarkNewObject(reinterpret_cast<BaseObject*>(addr));
// #endif
    } else {
        return nullptr;
    }
#if defined(__OHOS__) && (__OHOS__ == 1)
    if (CjAllocData::GetCjAllocData()->IsRecording()) {
        CjAllocData::GetCjAllocData()->RecordAllocNodes(ti, size);
    }
#endif
    return Cast<MObject>(addr);
}

MObject* MObject::NewFinalizer(const TypeInfo* ti, MSize size)
{
    CHECK_DETAIL(ti->IsObjectType() == true, "must be object class.");
    auto addr = HeapManager::Allocate(size);
    if (LIKELY(addr != NULL_ADDRESS)) {
        (void)SetClassInfo(addr, const_cast<TypeInfo*>(ti));
        reinterpret_cast<BaseObject*>(addr)->OnFinalizerCreated();
    } else {
        return nullptr;
    }
#if defined(__OHOS__) && (__OHOS__ == 1)
    if (CjAllocData::GetCjAllocData()->IsRecording()) {
        CjAllocData::GetCjAllocData()->RecordAllocNodes(ti, size);
    }
#endif
    return Cast<MObject>(addr);
}
} // namespace MapleRuntime
