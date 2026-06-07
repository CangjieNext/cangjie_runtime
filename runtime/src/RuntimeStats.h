// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RUNTIME_STATS_H
#define MRT_RUNTIME_STATS_H

#include <cstdint>

namespace MapleRuntime {
void RecordNewObjectStat();
void RecordNewArrayStat();
void RecordTakeRegionStat();
void AddTakeRegionDurationStat(uint64_t durationNs);
void RecordCopyGCAllocatorEntryStat();
void RecordMSFastAllocStat();
void RecordMSSlowAllocStat();
void RecordLargeObjectAllocStat();
void RecordReadBarrierEntryStat();
void RecordReadGenericStat();
void RecordWriteBarrierEntryStat();
} // namespace MapleRuntime

#endif // MRT_RUNTIME_STATS_H
