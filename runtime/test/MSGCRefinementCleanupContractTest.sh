#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ROOT_SNAPSHOT="$ROOT_DIR/src/Heap/MCollector/MSRootSnapshot.h"
MARK_BATCH="$ROOT_DIR/src/Heap/MCollector/MSMarkBatch.h"
WORKER_CONTROLLER="$ROOT_DIR/src/Heap/MCollector/MSGCWorkerPoolController.h"
RESOURCES_HEADER="$ROOT_DIR/src/Heap/Collector/CollectorResources.h"
RESOURCES_SOURCE="$ROOT_DIR/src/Heap/Collector/CollectorResources.cpp"
BACKGROUND_HEADER="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.h"
BACKGROUND_SOURCE="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"
CENTRAL_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
CENTRAL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
ALLOC_BUFFER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

require_absent() {
    local pattern=$1
    shift
    if rg -n "$pattern" "$@"; then
        echo "obsolete MSGC refinement symbol remains: $pattern" >&2
        exit 1
    fi
}

require_pattern() {
    local pattern=$1
    shift
    if ! rg -q "$pattern" "$@"; then
        echo "missing MSGC refinement boundary: $pattern" >&2
        exit 1
    fi
}

# Root snapshots publish through one closed staging boundary; exposing the raw
# vector would permit bypassing deduplication and publication.
require_absent 'Roots\(\)' "$ROOT_SNAPSHOT"
require_pattern 'void DrainRootsTo\(' "$ALLOC_BUFFER"
require_pattern 'buffer\.DrainRootsTo' "$COLLECTOR_SOURCE"

# Mark batches are initialized atomically from their epoch/bitmap context.
require_absent 'SetMarkEpoch\(' "$MARK_BATCH"
require_absent 'SetBitmapPlaneBases\(' "$MARK_BATCH"
require_pattern 'void SetBitmapContext\(const MSMarkBitmapContext& context\)' "$MARK_BATCH"

# MARK has no special worker-pool acquisition path beyond the generic owner API.
require_absent 'AcquireMark\(' "$WORKER_CONTROLLER" "$RESOURCES_HEADER" "$RESOURCES_SOURCE"
require_absent 'AcquireMSGCWorkerPoolForMark' "$RESOURCES_HEADER" "$RESOURCES_SOURCE"

# One sweep credit source covers both background and helper workers, while the
# pressure enum is stored with its real type.
require_absent 'RecordBackgroundSweep' "$CENTRAL_HEADER" "$CENTRAL_SOURCE"
require_absent 'RecordHelperSweep' "$CENTRAL_HEADER" "$CENTRAL_SOURCE"
require_pattern 'RecordNonAssistSweep' "$CENTRAL_HEADER" "$CENTRAL_SOURCE"
require_pattern 'std::atomic<SweepPressureMode> pressureMode' "$CENTRAL_HEADER"

# Slice completion is one stateful operation.  The helper owns the common
# deadline calculation and completion refresh; callers only retain their
# distinct serial/parallel execution paths.
require_absent 'SweepDispatchState' "$BACKGROUND_HEADER" "$BACKGROUND_SOURCE"
require_absent 'bool HasPendingShrinkWork\(\)' "$BACKGROUND_HEADER"
require_absent 'BackgroundProcessor::HasPendingShrinkWork\(' "$BACKGROUND_SOURCE"
require_pattern 'struct MSSweepSliceWindow' "$BACKGROUND_SOURCE"
require_pattern 'BuildMSSweepSliceWindow' "$BACKGROUND_SOURCE"
require_pattern 'uint64_t ComputeMSSweepSliceBudgetUs\(uint64_t baseBudgetUs' \
    "$ROOT_DIR/src/Heap/MAllocator/MSParallelSweepPolicy.h"
require_absent 'static_cast<size_t>\(GetSweepBaseBudgetUs\(\)\)' "$BACKGROUND_SOURCE"
require_pattern 'bool CompleteSweepSlice\(' "$BACKGROUND_HEADER" "$BACKGROUND_SOURCE"
require_pattern 'bool BackgroundProcessor::RefreshSweepDispatch\(\)' "$BACKGROUND_SOURCE"

# Class/scan traversal is shared by ordinary background and helper sweep.
require_pattern 'bool SweepClassOrder\(' "$CENTRAL_HEADER" "$CENTRAL_SOURCE"

echo 'MSGC refinement cleanup contracts: PASS'
