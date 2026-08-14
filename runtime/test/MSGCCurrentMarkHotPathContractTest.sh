#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BATCH="$ROOT_DIR/src/Heap/MCollector/MSMarkBatch.h"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SCHEDULER="$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.cpp"
SCHEDULER_HEADER="$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.h"

rg -q 'uint32_t occupiedMask_' "$BATCH"
rg -q '__builtin_ctz' "$BATCH"
if rg -q 'entry\.Reset\(\)' "$BATCH"; then
    echo 'GC MarkWordBatch still clears every resident entry on flush' >&2
    exit 1
fi

rg -q 'struct MSMarkPublicationContext' "$BATCH"
rg -q 'MSMarkPublicationContext& publication' "$COLLECTOR_HEADER" "$COLLECTOR"
rg -q 'FlushMarkPublication' "$COLLECTOR_HEADER" "$COLLECTOR"
rg -q 'MSMarkPublicationContext publication_' "$BATCH"
rg -q 'batch\.Publication\(\)' "$COLLECTOR"

if rg -q 'markBatchEpoch_|markBatchAllocPlaneBase_|markBatchMarkPlaneBase_' \
    "$COLLECTOR_HEADER" "$COLLECTOR"; then
    echo 'collector still caches the published MarkWordBatch bitmap context' >&2
    exit 1
fi
rg -q 'InitializeGCMarkBatch' "$COLLECTOR_HEADER" "$COLLECTOR"
rg -q 'CJ_MRT_MSGCFastPathState.currentMarkEpoch' "$COLLECTOR"

region_task="$(sed -n '/void MarkSweepCollector::RunRegionTask/,/void MarkSweepCollector::RunObjectTask/p' "$COLLECTOR")"
object_task="$(sed -n '/void MarkSweepCollector::RunObjectTask/,/void MarkSweepCollector::RunSchedulerTask/p' "$COLLECTOR")"
printf '%s\n' "$region_task" | rg -q 'typeInfo->IsWeakRefType\(\)'
printf '%s\n' "$object_task" | rg -q 'typeInfo->IsWeakRefType\(\)'
if printf '%s\n%s\n' "$region_task" "$object_task" | rg -q 'obj->IsWeakRef\(\)'; then
    echo 'CurrentMark reloads TypeInfo through BaseObject::IsWeakRef' >&2
    exit 1
fi

rg -q 'size_t& regionProbeCursor' "$SCHEDULER_HEADER" "$SCHEDULER"
if rg -q 'std::atomic<size_t> regionProbeCursor_' "$SCHEDULER_HEADER"; then
    echo 'GC main/helpers still contend on a shared probe cursor' >&2
    exit 1
fi

rg -q 'OutstandingWorkHint' "$SCHEDULER_HEADER" "$SCHEDULER" "$COLLECTOR"
rg -q 'MSMarkControllerState markController' "$COLLECTOR"
rg -q 'AcquireMarkWorkerPool' "$COLLECTOR_HEADER" "$COLLECTOR"
rg -q 'UpdateMarkController' "$COLLECTOR_HEADER" "$COLLECTOR"
rg -q 'FinishSchedulerMarkLoop' "$COLLECTOR_HEADER" "$COLLECTOR"
rg -q 'MS_MARK_CONTROL_SLICES_PER_UPDATE' "$COLLECTOR"
rg -q 'markController\.Observe\(' "$COLLECTOR"
rg -q 'markController\.HelperTarget\(\)' "$COLLECTOR"
rg -q 'markController\.AssistMultiplier\(\)' "$COLLECTOR"

mark_loop="$(sed -n '/void MarkSweepCollector::RunSchedulerMarkLoop/,/size_t MarkSweepCollector::AcquireMarkWorkerPool/p' "$COLLECTOR")"
for token in HasReachedMarkSchedulerQuiescence HasReachedMarkTimeBudget; do
    if ! printf '%s\n' "$mark_loop" | rg -q "$token"; then
        echo "RunSchedulerMarkLoop must use the named $token exit predicate" >&2
        exit 1
    fi
done
for token in initialOutstanding initialOutstandingWork activeHelpers; do
    if printf '%s\n' "$mark_loop" | rg -q "$token"; then
        echo "RunSchedulerMarkLoop retains one-use setup variable: $token" >&2
        exit 1
    fi
done

mark_controller="$(sed -n '/void MarkSweepCollector::UpdateMarkController/,/void MarkSweepCollector::FinishSchedulerMarkLoop/p' "$COLLECTOR")"
if printf '%s\n' "$mark_controller" | rg -q 'Mark elapsed pressure:'; then
    echo 'Mark controller must combine pressure details into its one control log' >&2
    exit 1
fi
if [[ "$(printf '%s\n' "$mark_controller" | rg -c 'VLOG\(REPORT')" -ne 1 ]]; then
    echo 'Mark controller must emit one log when control changes' >&2
    exit 1
fi
if rg -q 'ComputeMSMarkDynamicAssistRatioQ16\(' "$COLLECTOR"; then
    echo 'CurrentMark still derives Assist directly from instantaneous task count' >&2
    exit 1
fi
if rg -q 'ComputeMSMarkOutstandingLevel|MS_MARK_OUTSTANDING_TASKS_PER_LEVEL' \
    "$ROOT_DIR/src/Heap/MCollector/MSWorkUnit.h"; then
    echo 'obsolete eight-task CurrentMark level policy remains' >&2
    exit 1
fi

echo 'MSGC CurrentMark hot-path optimization contract holds'
