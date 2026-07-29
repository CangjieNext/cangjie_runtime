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
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSScanClass.h"
#include "Mutator/ThreadLocal.h"
#endif
namespace MapleRuntime {
MObject* MObject::NewObject(TypeInfo* ti, MSize size, AllocType allocType)
{
#ifdef MRT_USE_MARK_SWEEP
    ThreadLocalData* local = ThreadLocal::GetThreadLocalData();
    auto addr = HeapManager::AllocateMS(size, allocType, ClassifyMSScanClass(ti), local);
#else
    auto addr = HeapManager::Allocate(size, allocType);
#endif
    if (LIKELY(addr != NULL_ADDRESS)) {
        // MSGC reserves a virtual heap larger than its initially committed
        // prefix. Heap::IsHeapAddress() follows the committed heap boundary,
        // while an allocation from a later committed segment is nevertheless
        // a valid MSGC heap address. Use the virtual-reservation range in MS
        // mode so this diagnostic cannot reject a valid heap growth result.
#ifdef MRT_USE_MARK_SWEEP
        bool isHeapAddress = MSRegionInfoArray::IsHeapAddress(
            static_cast<uintptr_t>(addr));
#else
        bool isHeapAddress = Heap::IsHeapAddress(addr);
#endif
        if (UNLIKELY(!isHeapAddress)) {
            LOG(RTLOG_FATAL, "[MS-GC] NewObject: Allocate returned non-heap addr=0x%zx, "
                "size=%u, allocType=%d, ti=%p",
                addr, size, (int)allocType, (void*)ti);
        }
        (void)SetClassInfo(addr, ti);
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
    auto addr = HeapManager::AllocateMS(
        size, AllocType::PINNED_OBJECT, ClassifyMSScanClass(ti), local);
#else
    auto addr = HeapManager::Allocate(size, AllocType::PINNED_OBJECT);
#endif
    if (LIKELY(addr != NULL_ADDRESS)) {
        (void)SetClassInfo(addr, ti);
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
#ifdef MRT_USE_MARK_SWEEP
    ThreadLocalData* local = ThreadLocal::GetThreadLocalData();
    auto addr = HeapManager::AllocateMS(
        size, AllocType::MOVEABLE_OBJECT, ClassifyMSScanClass(ti), local);
#else
    auto addr = HeapManager::Allocate(size);
#endif
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
