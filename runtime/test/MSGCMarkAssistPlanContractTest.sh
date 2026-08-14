#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
PACING_HEADER="$ROOT_DIR/src/Heap/MCollector/MSAssistPacing.h"

for symbol in 'struct MSMarkAssistPlan' 'class MSMarkAssistPacing'; do
    if ! rg -q "$symbol" "$PACING_HEADER"; then
        echo "MSAssistPacing must own $symbol" >&2
        exit 1
    fi
done

if rg -q 'class MSMarkAssistPacer' "$PACING_HEADER"; then
    echo "MSMarkAssistPacing must keep density EWMA private" >&2
    exit 1
fi

if rg -q 'MSMarkAssistActivation' "$PACING_HEADER"; then
    echo "MSAssistPacing must publish the rate internally, not expose activation bookkeeping" >&2
    exit 1
fi

if ! rg -Uq 'MSMarkAssistPlan ActivateForCurrentCycle\(' "$PACING_HEADER"; then
    echo "MSAssistPacing activation must return only the reusable cycle plan" >&2
    exit 1
fi

if rg -q 'struct MSMarkAssistPlan|MSWorkUnitPacer markPacer_|nextMarkAssistPlan_' \
    "$COLLECTOR_HEADER"; then
    echo "MarkSweepCollector must not own Mark assist planning state" >&2
    exit 1
fi

if ! rg -q 'MSMarkAssistPacing markAssistPacing_;' "$COLLECTOR_HEADER"; then
    echo "MarkSweepCollector must retain one MSAssistPacing state object" >&2
    exit 1
fi

update_stats="$(sed -n '/void MarkSweepCollector::UpdateGCStats()/,/^}/p' "$COLLECTOR_SOURCE")"
if ! printf '%s\n' "$update_stats" | rg -q 'markAssistPacing_\.UpdateForCompletedCycle\('; then
    echo "MSGC must delegate cycle feedback to MSAssistPacing" >&2
    exit 1
fi

if rg -q 'UpdateMarkAssistPlan\(|ActivateMarkAssistForCurrentCycle\(' \
    "$COLLECTOR_HEADER" "$COLLECTOR_SOURCE"; then
    echo "MarkSweepCollector must not calculate Mark assist plans locally" >&2
    exit 1
fi

concurrent_mark="$(sed -n '/void MarkSweepCollector::ConcurrentMark()/,/^}/p' "$COLLECTOR_SOURCE")"
if ! printf '%s\n' "$concurrent_mark" | rg -q 'markAssistPacing_\.ActivateForCurrentCycle'; then
    echo "MSGC ConcurrentMark must activate assist through MSAssistPacing" >&2
    exit 1
fi

if printf '%s\n' "$concurrent_mark" | rg -q 'ComputeMSMarkInitialAssistMultiplier|SaturatingMSWUMul'; then
    echo "MSGC ConcurrentMark must not calculate assist rates locally" >&2
    exit 1
fi

update_controller="$(sed -n '/void MarkSweepCollector::UpdateMarkController(/,/^}/p' "$COLLECTOR_SOURCE")"
if printf '%s\n' "$update_controller" | rg -q 'ComputeMSMarkAssistRate|GetPublishedRateScaled'; then
    echo "MSGC UpdateMarkController must delegate rate calculation and publication to MSAssistPacing" >&2
    exit 1
fi

if printf '%s\n' "$update_stats" | rg -q 'ComputeMSAssistRatioQ16|markPacer_'; then
    echo "MSGC UpdateGCStats must not calculate assist rates locally" >&2
    exit 1
fi

assist_state="$(sed -n '/Mark assist pacing and publication state/,/Scheduler progress epoch and helper control/p' "$COLLECTOR_HEADER")"
for member in markAssistPacing_ activeCollector_; do
    if ! printf '%s\n' "$assist_state" | rg -q "$member"; then
        echo "MSGC mark assist state must keep $member in one documented group" >&2
        exit 1
    fi
done

if rg -q 'assistWorkPerByteScaled_|markAssistCompletedWU_' \
    "$COLLECTOR_HEADER" "$COLLECTOR_SOURCE"; then
    echo "MarkSweepCollector must not retain assist pacing atomics" >&2
    exit 1
fi

if rg -q 'markPacerDurationUs_' "$COLLECTOR_HEADER" "$COLLECTOR_SOURCE"; then
    echo "MSGC must not retain a duplicate ConcurrentMark duration state" >&2
    exit 1
fi

echo "MSGC next-cycle mark assist plan contract holds"
