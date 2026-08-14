#!/usr/bin/env bash
# Contract checks for the second MSGC cleanup pass.
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
root_dir=$(cd "$script_dir/.." && pwd)
collector="$root_dir/src/Heap/MCollector/MarkSweepCollector.cpp"
assist="$root_dir/src/Heap/MCollector/MSAssistPacing.h"
worker_control="$root_dir/src/Heap/MCollector/MSMarkWorkerControl.h"
parallel_sweep="$root_dir/src/Heap/MAllocator/MSParallelSweepPolicy.h"
sweep_pressure="$root_dir/src/Heap/MAllocator/MSSweepPressurePolicy.h"
scavenge="$root_dir/src/Heap/MCollector/MSScavengeWatermarkPolicy.h"
adaptive="$root_dir/src/Heap/MCollector/MSAdaptiveHeapGrowthPolicy.h"

fail() {
    echo "MSGC second refinement contract failed: $1" >&2
    exit 1
}

require_absent() {
    local pattern=$1
    local file=$2
    if rg -q "$pattern" "$file"; then
        fail "obsolete pattern '$pattern' remains in ${file#$root_dir/}"
    fi
}

require_present() {
    local pattern=$1
    local file=$2
    if ! rg -q "$pattern" "$file"; then
        fail "required pattern '$pattern' is missing from ${file#$root_dir/}"
    fi
}

require_absent 'void SetMarkEpoch\(' "$collector"
require_absent 'void SetBitmapPlaneBases\(' "$collector"

require_absent 'inline size_t SaturatingMultiply\(' "$parallel_sweep"
require_present 'SaturatingMultiplyMSSweepBytes' "$sweep_pressure"

require_absent 'MSScavengeThresholdDecision|EvaluateMSScavengeThreshold' "$scavenge"
require_present 'ComputeMSScavengeThreshold' "$scavenge"

require_absent 'MSAdaptiveHeapGrowthAction' "$adaptive"
adaptive_decision=$(awk '/struct MSAdaptiveHeapGrowthDecision \{/,/^};/' "$adaptive")
if printf '%s\n' "$adaptive_decision" | rg -q 'double gcRatio|uint8_t (cooldown|stableWorkingSet)|MSAdaptive.* action|bool (memoryPressure|criticalPressure)'; then
    fail "MSAdaptiveHeapGrowthDecision still exposes policy-internal fields"
fi

require_absent 'ComputeMSAssistRatioQ16|ApplyMSAssistDisableOverride' "$assist"
require_present 'ResolveMSMarkBaseAssistRatioQ16' "$assist"

require_present 'size_t GetTargetHelpers\(\) const' "$worker_control"
mark_assist=$(awk '/size_t MarkSweepCollector::MSMarkAssist\(/,/^}/' "$collector")
if printf '%s\n' "$mark_assist" | rg -q 'GetSnapshot\('; then
    fail "MSMarkAssist still takes a full worker-control snapshot"
fi
if ! printf '%s\n' "$mark_assist" | rg -q 'GetTargetHelpers\('; then
    fail "MSMarkAssist does not use the target-helper accessor"
fi

start_workers=$(awk '/bool MarkSweepCollector::StartMarkWorkers\(/,/^}/' "$collector")
if ! printf '%s\n' "$start_workers" | rg -q 'stagedTasks'; then
    fail "StartMarkWorkers does not stage helper tasks before submission"
fi
if printf '%s\n' "$start_workers" | rg -q 'AddWork\(new \(std::nothrow\)'; then
    fail "StartMarkWorkers can submit a partially constructed worker set"
fi

echo "MSGC second refinement contract checks passed"
