// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RSetUpdateBarrier.h"
#include "Allocator/RegionInfo.h"

namespace MapleRuntime {

void RSetUpdateBarrier::WriteReference(BaseObject* obj, BaseObject* field, BaseObject* target)
{
    if (!Heap::IsHeapAddress(obj)) { return; }
    RegionInfo* objRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    if (!objRegion->IsOldRegion()) {
        return;
    }
    RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(target));
    rset_.Record(targetRegion, reinterpret_cast<uintptr_t>(field));
}

} // namespace MapleRuntime
