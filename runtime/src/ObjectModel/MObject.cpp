// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Base/Log.h"
#include "MObject.inline.h"
#include "Heap/Heap.h"
#ifdef MRT_USE_MARK_SWEEP
#include "Heap/MAllocator/MSRegionInfo.h"
#include "Heap/MAllocator/MSScanClass.h"
#include "Mutator/ThreadLocal.h"
#endif
namespace MapleRuntime {
MObject* MObject::NewObjectImpl(TypeInfo* ti, MSize size, AllocType allocType,
                                bool registerFinalizer)
{
#ifdef MRT_USE_MARK_SWEEP
    ThreadLocalData* local = ThreadLocal::GetThreadLocalData();
    auto addr = HeapManager::AllocateMS(size, allocType, ClassifyMSScanClass(ti), local);
#else
    auto addr = HeapManager::Allocate(size, allocType);
#endif
    if (UNLIKELY(addr == NULL_ADDRESS)) {
        return nullptr;
    }

    // MSGC reserves a virtual heap larger than its initially committed prefix,
    // so its diagnostic must check the reservation rather than the committed
    // boundary used by the moving collector.
#ifdef MRT_USE_MARK_SWEEP
    bool isHeapAddress = MSRegionInfoArray::IsHeapAddress(static_cast<uintptr_t>(addr));
#else
    bool isHeapAddress = Heap::IsHeapAddress(addr);
#endif
    if (UNLIKELY(!isHeapAddress)) {
        LOG(RTLOG_FATAL, "NewObject: allocator returned non-heap addr=0x%zx, "
            "size=%u, allocType=%d, ti=%p",
            addr, size, static_cast<int>(allocType), static_cast<void*>(ti));
    }

    BaseObject* object = SetClassInfo(addr, ti);
    if (registerFinalizer) {
        object->OnFinalizerCreated();
    }
    return Cast<MObject>(addr);
}

MObject* MObject::NewObject(TypeInfo* ti, MSize size, AllocType allocType)
{
    return NewObjectImpl(ti, size, allocType, false);
}

MObject* MObject::NewPinnedObject(TypeInfo* ti, MSize size)
{
    CHECK_DETAIL(ti->IsObjectType() == true, "must be object class.");
    return NewObjectImpl(ti, size, AllocType::PINNED_OBJECT, false);
}

MObject* MObject::NewFinalizer(const TypeInfo* ti, MSize size)
{
    CHECK_DETAIL(ti->IsObjectType() == true, "must be object class.");
    return NewObjectImpl(const_cast<TypeInfo*>(ti), size,
                         AllocType::MOVEABLE_OBJECT, true);
}
} // namespace MapleRuntime
