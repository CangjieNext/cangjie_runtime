// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Base/Log.h"
#include "MObject.inline.h"
#include "Inspector/CjAllocData.h"
#include "Heap/Heap.h"
namespace MapleRuntime {
MObject* MObject::NewObject(TypeInfo* ti, MSize size, AllocType allocType)
{
    return NewObject(ti, size, allocType, nullptr);
}

MObject* MObject::NewObject(TypeInfo* ti, MSize size, AllocType allocType, ThreadLocalData* local)
{
    auto addr = HeapManager::Allocate(size, allocType, local);
    if (LIKELY(addr != NULL_ADDRESS)) {
        // Debug: validate allocated address is in heap range
        if (UNLIKELY(!Heap::IsHeapAddress(addr))) {
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
    return NewPinnedObject(ti, size, nullptr);
}

MObject* MObject::NewPinnedObject(TypeInfo* ti, MSize size, ThreadLocalData* local)
{
    CHECK_DETAIL(ti->IsObjectType() == true, "must be object class.");
    auto addr = HeapManager::Allocate(size, AllocType::PINNED_OBJECT, local);
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
    return NewFinalizer(ti, size, nullptr);
}

MObject* MObject::NewFinalizer(const TypeInfo* ti, MSize size, ThreadLocalData* local)
{
    CHECK_DETAIL(ti->IsObjectType() == true, "must be object class.");
    auto addr = HeapManager::Allocate(size, AllocType::MOVEABLE_OBJECT, local);
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
