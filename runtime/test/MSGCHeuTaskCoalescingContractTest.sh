#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TASK_QUEUE_HEADER="$ROOT_DIR/src/Heap/Collector/TaskQueue.h"
TASK_QUEUE_SOURCE="$ROOT_DIR/src/Heap/Collector/TaskQueue.cpp"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/Collector/Collector.h"
COLLECTOR_PROXY_HEADER="$ROOT_DIR/src/Heap/Collector/CollectorProxy.h"
COLLECTOR_RESOURCES_HEADER="$ROOT_DIR/src/Heap/Collector/CollectorResources.h"
COLLECTOR_RESOURCES_SOURCE="$ROOT_DIR/src/Heap/Collector/CollectorResources.cpp"
MS_COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
MS_COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
REGION_MANAGER_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
SWEEP_POLICY_HEADER="$ROOT_DIR/src/Heap/MCollector/MSAssistPacing.h"
BACKGROUND_HEADER="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.h"
BACKGROUND_SOURCE="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"

if ! rg -q 'bool Push\(const T& task\)' "$TASK_QUEUE_HEADER" ||
   ! rg -q 'return state_\.Push\(task\.GetPriority\(\), task\.IsOverriding\(\)\);' "$TASK_QUEUE_HEADER"; then
    echo "lockless async queue must report whether a task was accepted" >&2
    exit 1
fi

if ! rg -q 'bool EnqueueAsync\(const T& task\)' "$TASK_QUEUE_HEADER" ||
   ! rg -q 'bool accepted = asyncTaskQueue\.Push\(task\);' "$TASK_QUEUE_HEADER"; then
    echo "task queue must propagate async enqueue acceptance" >&2
    exit 1
fi

if ! rg -q 'enum class AsyncGCEnqueueResult' "$COLLECTOR_RESOURCES_HEADER" ||
   ! rg -q 'TryRequestAsyncGC\(GCReason reason\)' "$COLLECTOR_RESOURCES_HEADER"; then
    echo "collector resources must expose typed async enqueue results" >&2
    exit 1
fi

if ! rg -Uq 'bool shouldContinue = task\.Execute\(owner\);[[:space:]]*if \(task\.GetTaskIndex\(\) == GCTask::ASYNC_TASK_INDEX\) \{[[:space:]]*asyncTaskQueue\.Complete\(task\);' \
    "$TASK_QUEUE_HEADER"; then
    echo "async task running state must be cleared only after Execute returns" >&2
    exit 1
fi

for source_file in "$TASK_QUEUE_SOURCE" "$COLLECTOR_HEADER" "$COLLECTOR_PROXY_HEADER" \
                   "$MS_COLLECTOR_SOURCE" "$MS_COLLECTOR_HEADER"; do
    if rg -q 'ShouldExecuteQueuedRequest' "$source_file"; then
        echo "queued HEU must execute without the obsolete dequeue-time gate: $source_file" >&2
        exit 1
    fi
done

for source_file in "$SWEEP_POLICY_HEADER" "$MS_COLLECTOR_SOURCE" "$REGION_MANAGER_SOURCE" \
                   "$BACKGROUND_HEADER" "$BACKGROUND_SOURCE" "$COLLECTOR_RESOURCES_SOURCE"; do
    if rg -q 'EvaluateMSHeuExecution|MSHeuExecutionDecision|DEFER_FOR_SWEEP|deferHeu|DeferredHeuOrigin|deferredHeu|NotifyDeferredHeuRetry|ClearDeferredHeuRequest' \
        "$source_file"; then
        echo "HEU defer/retry state must be removed: $source_file" >&2
        exit 1
    fi
done

maybe_notify_body="$(awk '
    /void MSRegionManager::MaybeNotifyMark\(\)/ { capture = 1 }
    capture { print }
    capture && /^}/ { exit }
' "$REGION_MANAGER_SOURCE")"
if printf '%s\n' "$maybe_notify_body" | rg -q 'RefreshSweepDebtSnapshot'; then
    echo "MaybeNotifyMark must consume the published Sweep pressure without refreshing it" >&2
    exit 1
fi
threshold_line="$(printf '%s\n' "$maybe_notify_body" | rg -n 'MSMarkTriggerDecision markDecision = EvaluateMSMarkTrigger' | sed -n '1s/:.*//p')"
mode_line="$(printf '%s\n' "$maybe_notify_body" | rg -n 'pool->GetSweepPressureMode\(\)' | sed -n '1s/:.*//p')"
pressure_line="$(printf '%s\n' "$maybe_notify_body" | rg -n 'pressureMode != SweepPressureMode::LOW' | sed -n '1s/:.*//p')"
notify_line="$(printf '%s\n' "$maybe_notify_body" | rg -n 'NotifyToParallelSweep\(\)' | sed -n '1s/:.*//p')"
request_line="$(printf '%s\n' "$maybe_notify_body" | rg -n 'TryRequestAsyncGC\(GC_REASON_HEU\)' | sed -n '1s/:.*//p')"
if [[ -z "$threshold_line" || -z "$mode_line" || -z "$pressure_line" ||
      -z "$notify_line" || -z "$request_line" ||
      "$threshold_line" -ge "$mode_line" ||
      "$mode_line" -ge "$pressure_line" || "$pressure_line" -ge "$notify_line" ||
      "$notify_line" -ge "$request_line" ]]; then
    echo "MaybeNotifyMark must evaluate one aggregate mark trigger, wake pressured Sweep, then submit HEU" >&2
    exit 1
fi
if printf '%s\n' "$maybe_notify_body" |
    rg -q 'GetSweptHeapBytesReference|markPressureProbeInProgress_|lastHeuRequestNs_'; then
    echo "MaybeNotifyMark must not perform a reference confirmation or time cooldown" >&2
    exit 1
fi

if ! rg -Uq 'bool BackgroundProcessor::NotifyToParallelSweep\(\)[[:space:][:print:]]*\{[[:space:][:print:]]*ForceHighSweepPressure\(\)[[:space:][:print:]]*return RequestWork\(shouldParallelSweep_\);[[:space:]]*\}' \
    "$BACKGROUND_SOURCE"; then
    echo "NotifyToParallelSweep must force HIGH and then perform a pure Sweep wakeup" >&2
    exit 1
fi

if rg -q 'TryRequestAsyncGC\(GC_REASON_HEU\)' "$BACKGROUND_SOURCE"; then
    echo "parallel Sweep must not resubmit HEU" >&2
    exit 1
fi

echo "MSGC HEU pending/running coalescing contract holds"
