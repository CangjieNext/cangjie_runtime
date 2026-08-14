// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_WORK_UNIT_H
#define MRT_MS_WORK_UNIT_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>
#include <limits>

namespace MapleRuntime {

// This header intentionally defines only task-local Mark work accounting and
// scheduler slice budgets. Assist rates, multipliers, pressure, and feedback
// state are owned by MSAssistPacing.h.
constexpr uint64_t MS_WU_RESOLVED_REF_COST = 1;
constexpr uint64_t MS_WU_MARK_WORD_RMW_COST = 8;
constexpr uint64_t MS_WU_SCAN_WORD_COST = 1;
constexpr uint64_t MS_WU_MAIN_WALL_BUDGET_US = 100;
constexpr uint64_t MS_WU_HELPER_WALL_BUDGET_US = 100;
constexpr uint64_t MS_WU_UNLIMITED_BUDGET =
    std::numeric_limits<uint64_t>::max();

// Cross-task WorkUnit aggregation is shared by the scheduler, collector and
// assist accounting. Keep the arithmetic with the unit definition rather than
// coupling generic scheduler code to pacing policy.
inline uint64_t SaturatingMSWUAdd(uint64_t lhs, uint64_t rhs)
{
    return rhs > std::numeric_limits<uint64_t>::max() - lhs
        ? std::numeric_limits<uint64_t>::max()
        : lhs + rhs;
}

struct MSWorkUnitCounter {
    uint64_t totalWU = 0;

    void AddResolvedRefs(uint64_t count = 1)
    {
        totalWU += count * MS_WU_RESOLVED_REF_COST;
    }

    void AddMarkWordRMW(uint64_t count = 1)
    {
        totalWU += count * MS_WU_MARK_WORD_RMW_COST;
    }

    void AddScanWords(uint64_t count = 1)
    {
        totalWU += count * MS_WU_SCAN_WORD_COST;
    }

    uint64_t Total() const
    {
        // This is a task-local counter. A task cannot process enough heap
        // objects to approach uint64_t overflow on the supported 64-bit heap,
        // so keep the per-reference path branch-free. Cross-task aggregation
        // uses the saturating helpers in MSAssistPacing.
        return totalWU;
    }

};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_WORK_UNIT_H
