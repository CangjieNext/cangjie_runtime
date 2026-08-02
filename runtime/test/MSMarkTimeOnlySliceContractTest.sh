#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
WORK_UNIT="$ROOT_DIR/src/Heap/MCollector/MSWorkUnit.h"

rg -q 'MS_WU_MAIN_WALL_BUDGET_US = 100' "$WORK_UNIT"
rg -q 'MS_WU_HELPER_WALL_BUDGET_US = 100' "$WORK_UNIT"
rg -q 'MS_WU_UNLIMITED_BUDGET' "$WORK_UNIT"

main_loop="$(sed -n '/void MarkSweepCollector::RunSchedulerMarkLoop/,/void MarkSweepCollector::StartMarkWorkers/p' "$COLLECTOR")"
helper_loop="$(sed -n '/void MarkSweepCollector::RunMarkWorker/,/size_t MarkSweepCollector::QuiesceMarkWorkers/p' "$COLLECTOR")"
assist_loop="$(sed -n '/size_t MarkSweepCollector::MSMarkAssist/,/void MarkSweepCollector::MarkTermination/p' "$COLLECTOR")"
region_task="$(sed -n '/void MarkSweepCollector::RunRegionTask/,/void MarkSweepCollector::RunObjectTask/p' "$COLLECTOR")"

if printf '%s\n' "$main_loop" | rg -q 'ComputeMSMarkSliceBudget'; then
    echo 'GC main still derives a finite WU slice budget' >&2
    exit 1
fi
if printf '%s\n' "$helper_loop" | rg -q 'ComputeMSMarkSliceBudget'; then
    echo 'GC helper still derives a finite WU slice budget' >&2
    exit 1
fi
printf '%s\n' "$main_loop" | rg -q 'MS_WU_UNLIMITED_BUDGET'
printf '%s\n' "$helper_loop" | rg -q 'MS_WU_UNLIMITED_BUDGET'
printf '%s\n' "$main_loop" | rg -q 'mainCompletedWU = SaturatingMSWUAdd'
if [[ "$(printf '%s\n' "$main_loop" | rg -c 'cycleCompletedMarkWU_\.fetch_add')" -ne 1 ]]; then
    echo 'GC main must merge locally accumulated WU only once per Mark loop' >&2
    exit 1
fi
if printf '%s\n' "$helper_loop" | rg -q 'cycleCompletedMarkWU_\.fetch_add'; then
    echo 'GC helper still updates the shared WU counter per slice' >&2
    exit 1
fi
printf '%s\n' "$helper_loop" | rg -q 'markWorkerCompletedWU_\[workerId\] = helperCompletedWU'

# Mutator assist still pays bounded WU debt, and Region scanning retains its
# bitmap-word boundary check for that caller.
printf '%s\n' "$assist_loop" | rg -q 'budgetWU'
printf '%s\n' "$region_task" | rg -q 'work\.Total\(\) >= taskBudgetWU'

echo 'GC main/helper use the shared 100us time-only slice policy'
