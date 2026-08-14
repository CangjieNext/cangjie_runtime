// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <cassert>

#include "Heap/MAllocator/MSMarkedStats.h"

using MapleRuntime::MSMarkedStats;

int main()
{
    MSMarkedStats stats;
    stats.AddSlots(5, 16);
    stats.AddSlots(3, 64);
    stats.AddSlots(0, 128);
    stats.AddLargeObject(32768);
    stats.AddLargeObject(65536);

    assert(stats.liveBytes == 5 * 16 + 3 * 64 + 32768 + 65536);
    return 0;
}
