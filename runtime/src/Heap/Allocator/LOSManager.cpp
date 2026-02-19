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

    // Initialize the cartesian trees for free region management
    // Note: We use unit count relative to LOS, not global indices
    UnitCount losUnitCount = unitEnd - unitStart;
    releasedUnitTree.Init(losUnitCount);
    dirtyUnitTree.Init(losUnitCount);

    DLOG(REPORT, "LOSManager initialized: space [0x%zx, 0x%zx), units [%u, %u), capacity %zu bytes",
         losStart, losEnd, losUnitStart, losUnitEnd, GetCapacity());
}

} // namespace MapleRuntime
