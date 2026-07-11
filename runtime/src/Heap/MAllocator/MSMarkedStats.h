// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARKED_STATS_H
#define MRT_MS_MARKED_STATS_H

#include <cstddef>

namespace MapleRuntime {

struct MSMarkedStats {
    size_t objectCount = 0;
    size_t liveBytes = 0;

    void AddSlots(size_t markedSlots, size_t slotSize)
    {
        objectCount += markedSlots;
        liveBytes += markedSlots * slotSize;
    }

    void AddLargeObject(size_t objectBytes)
    {
        ++objectCount;
        liveBytes += objectBytes;
    }
};

} // namespace MapleRuntime

#endif // MRT_MS_MARKED_STATS_H
