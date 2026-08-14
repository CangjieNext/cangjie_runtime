#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PACING_HEADER="$ROOT_DIR/src/Heap/MCollector/MSAssistPacing.h"
PACING_SOURCE="$ROOT_DIR/src/Heap/MCollector/MSAssistPacing.cpp"
SWEEP_POLICY="$ROOT_DIR/src/Heap/MAllocator/MSSweepDebtPolicy.h"
ALLOC_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
ALLOC_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
CENTRAL_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
MCOLLECTOR_CMAKE="$ROOT_DIR/src/Heap/MCollector/CMakeLists.txt"
TERMINATION_POLICY="$ROOT_DIR/src/Heap/MCollector/MSMarkTerminationPolicy.h"

if [[ ! -f "$PACING_SOURCE" ]]; then
    echo "Mark/Sweep assist implementation must live in MSAssistPacing.cpp" >&2
    exit 1
fi

if rg -q '^#ifdef MRT_USE_MARK_SWEEP$' "$PACING_HEADER"; then
    echo "MSAssistPacing must remain directly includable by its pure unit tests" >&2
    exit 1
fi

if [[ -e "$SWEEP_POLICY" ]]; then
    echo "separate MSSweepDebtPolicy ownership must be removed" >&2
    exit 1
fi

for symbol in MSMarkAssistDebtState MSSweepAssistDebtState MSSweepCreditPool \
              MSSweepAssistResult; do
    if ! rg -q "${symbol}" "$PACING_HEADER"; then
        echo "MSAssistPacing must own ${symbol}" >&2
        exit 1
    fi
done

if rg -q 'class MSMarkAssistPacer' "$PACING_HEADER"; then
    echo "Mark density EWMA must remain private to MSMarkAssistPacing" >&2
    exit 1
fi

if ! rg -q 'MSMarkAssistDebtState markAssistDebt_;' "$ALLOC_HEADER" ||
   ! rg -q 'MSSweepAssistDebtState sweepAssistDebt_;' "$ALLOC_HEADER"; then
    echo "AllocBufferMS must retain only the two assist debt states" >&2
    exit 1
fi

if rg -q 'gcAssistDebtScaled_|gcAssistCreditWU_|sweepAssistDebtScaled_|sweepDebtGen_' \
    "$ALLOC_HEADER"; then
    echo "raw assist debt fields must move behind pacing state" >&2
    exit 1
fi

if rg -q 'gcAssistTelemetryDrainLock_|atomic_flag' "$ALLOC_HEADER"; then
    echo "single-owner assist accounting must not retain a misleading drain spin lock" >&2
    exit 1
fi

if ! rg -q 'UpdateMSMarkAssistDebt\(' "$ALLOC_SOURCE" ||
   ! rg -q 'UpdateMSSweepAssistDebt\(' "$ALLOC_SOURCE"; then
    echo "AllocBufferMS must delegate both debt paths to MSAssistPacing" >&2
    exit 1
fi

if ! rg -q 'void ClearDebtAndCredit\(\)' "$PACING_HEADER" ||
   ! rg -q 'state\.ClearDebtAndCredit\(\)' "$PACING_SOURCE"; then
    echo "Mark assist must discard stale debt without resetting its shard probe" >&2
    exit 1
fi

for symbol in ConsumeMSMarkAssistCredit AccountMSMarkAssistWork \
              AccumulateMSSweepDebtScaled PayMSSweepDebtRegions \
              MSAssistBaseRatioDecision EvaluateMSAssistBaseRatio \
              ComputeMSAssistBaseRatioScaled UpdateMSAssistBacklogEMA \
              ComputeMSAssistBacklogRatioScaled; do
    if rg -q "${symbol}" "$PACING_HEADER"; then
        echo "unused assist compatibility symbol must be removed: ${symbol}" >&2
        exit 1
    fi
done

if ! rg -q 'MSSweepCreditPool credit;' "$CENTRAL_HEADER"; then
    echo "CentralRegionPool must keep the shared Sweep credit state" >&2
    exit 1
fi

if ! rg -q 'void AccountSweepResult\(const MSSweepAssistResult& result\)' \
    "$PACING_HEADER" ||
   ! rg -Uq 'state\.AccountSweepResult\([[:space:]]*centralPool->MSSweepAssist\(' \
    "$PACING_SOURCE"; then
    echo "Sweep debt state must consume the complete sweep-assist result" >&2
    exit 1
fi

if ! rg -q 'MSAssistPacing\.cpp' "$MCOLLECTOR_CMAKE"; then
    echo "MCollector must compile MSAssistPacing.cpp" >&2
    exit 1
fi

if rg -q 'MSMarkPressurePolicy|ScaleMSAssistRatioForPressure|MSMarkCheckpointDecision' \
    "$TERMINATION_POLICY"; then
    echo "unused Mark assist policy must not remain under MarkTermination" >&2
    exit 1
fi

echo "MSGC Mark/Sweep assist pacing ownership contract holds"
