// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_SWEEP_DEBT_POLICY_H
#define MRT_MS_SWEEP_DEBT_POLICY_H

#ifdef MRT_USE_MARK_SWEEP

#include <algorithm>
#include <cstddef>
#include <cstdint>

// ============================================================================
// Sweep-debt policy (Step34 §14-19): backlog-aware HEU gating + tiered pressure.
//
// Pure, header-only computation so the waterline / pressure-mode logic can be
// reasoned about (and unit-tested) without dragging in MSCentralRegionPool.
//
// Concepts:
//   runway   = how much more allocation can happen before the next HEU trigger,
//              measured on the swept-region-bytes axis (threshold - sweptBytes).
//   waterline= hysteretic enter/exit thresholds so the mode does not flap at the
//              boundary (BACKLOG enter ratio > exit ratio; EMERGENCY enter > exit).
//   pressure = NORMAL / BACKLOG / EMERGENCY, scaling how hard background + assist
//              sweep repays debt. Semantics: "how far sweep debt has fallen behind
//              the runway we still allow allocation against" — not "hardPressure".
// ============================================================================
namespace MapleRuntime {

enum class SweepPressureMode : int {
    NORMAL = 0,
    BACKLOG = 1,
    EMERGENCY = 2,
};

struct SweepDebtParams {
    // BACKLOG enters when unswept > enter*runway, exits when unswept <= exit*runway.
    double backlogEnterRatio = 2.0;
    double backlogExitRatio = 1.0;
    // EMERGENCY (runway == 0) enters above enter regions, exits below exit regions.
    size_t emergencyEnterRegions = 64;
    size_t emergencyExitRegions = 16;
};

struct SweepDebtDecision {
    SweepPressureMode mode = SweepPressureMode::NORMAL;
    bool deferHeu = false;                  // defer this GC_REASON_HEU, repay first
    double assistRatioMultiplier = 1.0;     // scale sweepRegionsPerAlloc_
    double assistCapMultiplier = 1.0;       // scale msAssistSweepMaxRegionsPerCall
    double backgroundBudgetMultiplier = 1.0; // scale per-slice background time budget
};

// runway in regions on the swept-bytes axis the HEU trigger already uses.
inline size_t ComputeRunwayRegions(size_t thresholdBytes, size_t sweptBytes, size_t regionSize)
{
    if (regionSize == 0) {
        return 0;
    }
    size_t runwayBytes = thresholdBytes > sweptBytes ? (thresholdBytes - sweptBytes) : 0;
    return (runwayBytes + regionSize - 1) / regionSize;  // ceil
}

// Evaluate the pressure mode with hysteresis. prev is the previously latched mode.
inline SweepPressureMode EvaluateSweepPressure(size_t unsweptRegions, size_t runwayRegions,
                                               bool sweepDone, SweepPressureMode prev,
                                               const SweepDebtParams& p)
{
    if (sweepDone || unsweptRegions == 0) {
        return SweepPressureMode::NORMAL;
    }
    if (runwayRegions == 0) {
        // Runway exhausted: "above runway" loses meaning, gate purely on backlog size.
        bool wasEmergency = (prev == SweepPressureMode::EMERGENCY);
        size_t threshold = wasEmergency ? p.emergencyExitRegions : p.emergencyEnterRegions;
        return unsweptRegions > threshold ? SweepPressureMode::EMERGENCY : SweepPressureMode::NORMAL;
    }
    bool wasBacklog = (prev != SweepPressureMode::NORMAL);
    double ratio = wasBacklog ? p.backlogExitRatio : p.backlogEnterRatio;
    double threshold = ratio * static_cast<double>(runwayRegions);
    return static_cast<double>(unsweptRegions) > threshold ? SweepPressureMode::BACKLOG
                                                           : SweepPressureMode::NORMAL;
}

// Tier multipliers: NORMAL=1x, BACKLOG=4x, EMERGENCY=10x.
inline double SweepTierMultiplier(SweepPressureMode mode)
{
    switch (mode) {
        case SweepPressureMode::BACKLOG: return 4.0;
        case SweepPressureMode::EMERGENCY: return 10.0;
        case SweepPressureMode::NORMAL:
        default: return 1.0;
    }
}

inline SweepDebtDecision BuildSweepDebtDecision(SweepPressureMode mode, bool sweepDone)
{
    SweepDebtDecision d;
    d.mode = mode;
    double m = SweepTierMultiplier(mode);
    d.assistRatioMultiplier = m;
    d.assistCapMultiplier = m;
    d.backgroundBudgetMultiplier = m;
    // Only BACKLOG/EMERGENCY with outstanding debt defer the next HEU GC. At the
    // HEU gate runway is ~0, so this resolves to EMERGENCY in practice; the BACKLOG
    // tier mainly drives the assist/background multipliers away from the gate.
    d.deferHeu = (mode != SweepPressureMode::NORMAL) && !sweepDone;
    return d;
}

}  // namespace MapleRuntime

#endif  // MRT_USE_MARK_SWEEP
#endif  // MRT_MS_SWEEP_DEBT_POLICY_H
