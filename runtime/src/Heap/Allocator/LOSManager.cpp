// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Allocator/LOSManager.h"

#include "Base/LogFile.h"

namespace MapleRuntime {

void LOSManager::Initialize(uintptr_t start, uintptr_t end, UnitCount unitStart, UnitCount unitEnd)
{
    losStart = start;
    losEnd = end;
    losUnitStart = unitStart;
    losUnitEnd = unitEnd;

    // Initialize inactive zone at the end (LOS grows toward low address)
    losInactiveZone.store(losEnd, std::memory_order_relaxed);

    // Initialize FreeRegionManager for LOS space.
    // The FreeRegionManager's baseUnit was set to losUnitStart in constructor via
    // RegionManager::Initialize(). Here we just initialize the CartesianTrees
    // with the LOS unit count. The trees use local indices [0, losUnitCount),
    // and baseUnit offset is applied transparently when accessing RegionInfo.
    UnitCount losUnitCount = unitEnd - unitStart;
    losFreeMgr.Initialize(losUnitCount);

    DLOG(REPORT, "LOSManager initialized: space [0x%zx, 0x%zx), units [%u, %u), capacity %zu bytes, "
         "FreeRegionManager baseUnit=%u",
         losStart, losEnd, losUnitStart, losUnitEnd, GetCapacity(), losFreeMgr.GetBaseUnit());
}

} // namespace MapleRuntime
