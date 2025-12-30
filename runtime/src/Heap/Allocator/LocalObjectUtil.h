// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_LOCAL_OBJECT_UTIL_H
#define MRT_LOCAL_OBJECT_UTIL_H

#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
inline bool IsLocalObject(BaseObject* obj, const Mutator* mutator = nullptr)
{
    if (obj == nullptr) {
        return false;
    }
    RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    return theAllocator.GetLocalObjectAllocator().IsLocalAddress(reinterpret_cast<MAddress>(obj), mutator);
}

inline bool IsManagedObject(BaseObject* obj, const Mutator* mutator = nullptr)
{
    return Heap::IsHeapAddress(obj) || IsLocalObject(obj, mutator);
}
} // namespace MapleRuntime

#endif // MRT_LOCAL_OBJECT_UTIL_H
