#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
WORK_UNIT="$ROOT_DIR/src/Heap/MCollector/MSWorkUnit.h"

rg -q 'MS_WU_MAIN_WALL_BUDGET_US = 100' "$WORK_UNIT"
rg -q 'MS_WU_HELPER_WALL_BUDGET_US = 100' "$WORK_UNIT"
rg -q 'MS_WU_ASSIST_THRESHOLD = 2048' "$WORK_UNIT"
rg -q 'MS_WU_ASSIST_WALL_BUDGET_US = 100' "$WORK_UNIT"
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
printf '%s\n' "$assist_loop" | rg -q 'MS_WU_ASSIST_WALL_BUDGET_US'
printf '%s\n' "$main_loop" | rg -q 'mainCompletedWU = SaturatingMSWUAdd'
if [[ "$(printf '%s\n' "$main_loop" | rg -c 'cycleCompletedMarkWU_\.fetch_add')" -ne 2 ]]; then
    echo 'GC main and folded Assist WU must each merge only once per Mark loop' >&2
    exit 1
fi
printf '%s\n' "$main_loop" | rg -Uq '(?s)cycleStats_\.markAssistWU.*cycleCompletedMarkWU_\.fetch_add'
if printf '%s\n' "$helper_loop" | rg -q 'cycleCompletedMarkWU_\.fetch_add'; then
    echo 'GC helper still updates the shared WU counter per slice' >&2
    exit 1
fi
printf '%s\n' "$helper_loop" | rg -q 'markWorkerCompletedWU_\[workerId\] = helperWU'

# Mutator assist retains a finite outer WU slice, but an already claimed Region
# uses the same whole-task execution contract as GC main/helper.
printf '%s\n' "$assist_loop" | rg -q 'budgetWU'
printf '%s\n' "$assist_loop" | rg -q 'RunMarkAssistSlice'
printf '%s\n' "$assist_loop" | rg -Uq 'RunSchedulerTask\([[:space:]]*lease, work\)'
if printf '%s\n' "$region_task" | rg -q 'taskBudgetWU|budgetExpired|RequeueOwnedRegion'; then
    echo 'whole-Region execution still carries a dead finite-WU replacement path' >&2
    exit 1
fi

echo 'GC main/helper use the shared 100us time-only slice policy'
