// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MCollector/MSAssistPacing.h"

#include "Heap/MAllocator/MSAllocBuffer.h"
#include "Heap/MAllocator/MSCentralRegionPool.h"
#include "Heap/MCollector/MarkSweepCollector.h"

namespace MapleRuntime {

// The collector owns mark task admission and scanning. This allocation slow
// path owns only P-local accounting: charge bytes, use prior overpayment, then
// translate completed scheduler work back into debt/credit.
void UpdateMSMarkAssistDebt(MSMarkAssistDebtState& state,
                            AllocBufferMS& allocBuffer,
                            size_t allocatedBytes)
{
    MarkSweepCollector* collector = MarkSweepCollector::GetActiveCollector();
    if (collector == nullptr) {
        state.ClearDebtAndCredit();
        return;
    }

    uint64_t ratioScaled = collector->GetAssistWorkPerByteScaled();
    if (ratioScaled == 0) {
        state.ClearDebtAndCredit();
        return;
    }

    state.Accumulate(allocatedBytes, ratioScaled);
    state.ConsumeCredit();
    size_t debtWU = state.WholeDebtWU();
    if (debtWU < MS_WU_ASSIST_THRESHOLD) {
        return;
    }

    state.AccountCompletedWork(collector->MSMarkAssist(allocBuffer, debtWU));
}

// The central pool owns sweep-epoch admission, shared credits and actual
// sweeping. This allocation slow path keeps a P-local fractional Region debt
// and lets the pool report exactly how many whole Regions paid it.
void UpdateMSSweepAssistDebt(MSSweepAssistDebtState& state,
                             CentralRegionPool* centralPool,
                             size_t allocatedRegions,
                             uint8_t preferredCls)
{
    if (centralPool == nullptr || allocatedRegions == 0) {
        return;
    }

    state.ResetForGeneration(centralPool->GetSweepCycleId());
    if (centralPool->IsSweepDone() || !centralPool->HasPendingSweepWork()) {
        state.ClearDebt();
        return;
    }

    uint64_t ratioScaled = centralPool->GetSweepAssistRatioScaled();
    if (ratioScaled == 0) {
        state.ClearDebt();
        return;
    }

    state.Accumulate(allocatedRegions, ratioScaled);
    size_t debtRegions = state.WholeDebtRegions();
    if (debtRegions == 0) {
        return;
    }

    state.AccountSweepResult(
        centralPool->MSSweepAssist(debtRegions, preferredCls));
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
