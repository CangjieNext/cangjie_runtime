#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/MSEnumRootsWorkerSession.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MSEnumRootsWorkerSession.cpp"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
MANAGER="$ROOT_DIR/src/Mutator/MutatorManager.cpp"

[[ -f "$HEADER" && -f "$SOURCE" ]]
rg -q 'class MSEnumRootsWorkerSession' "$HEADER"
rg -q 'RunStage' "$HEADER"
rg -q 'WaitStage' "$HEADER"
rg -q 'Finish' "$HEADER"

enum_body="$(sed -n '/void MarkSweepCollector::EnumRoots/,/^}/p' "$COLLECTOR")"
printf '%s\n' "$enum_body" | rg -q 'enumRootsWorkerSession_\.Start'
printf '%s\n' "$enum_body" | rg -q 'TransitionAllMutatorsToGCPhase'
printf '%s\n' "$enum_body" | rg -q 'GCPhase::GC_PHASE_MS_MARK_SETUP, &enumRootsWorkerSession_'
printf '%s\n' "$enum_body" | rg -q 'PublishRootStaging'
printf '%s\n' "$enum_body" | rg -q 'enumRootsWorkerSession_\.Finish'

start_count="$(printf '%s\n' "$enum_body" | rg -c 'enumRootsWorkerSession_\.Start')"
finish_count="$(printf '%s\n' "$enum_body" | rg -c 'enumRootsWorkerSession_\.Finish')"
[[ "$start_count" -eq 1 && "$finish_count" -eq 1 ]]

ensure="$(sed -n '/void MutatorManager::EnsureMSMarkSetupPhaseTransition/,/^}/p' "$MANAGER")"
printf '%s\n' "$ensure" | rg -q 'workerSession->RunStage'
printf '%s\n' "$ensure" | rg -q 'workerSession->WaitStage'
if printf '%s\n' "$ensure" | rg -q 'threadPool->Start|threadPool->WaitFinish|AcquireMSGCWorkerPoolForEnumRoots'; then
    echo 'transition must reuse the collector-owned EnumRoots pool epoch' >&2
    exit 1
fi

rg -q 'threadPool_->Start\(\)' "$SOURCE"
rg -q 'threadPool_->WaitFinish\(\)' "$SOURCE"
[[ "$(rg -c 'threadPool_->Start\(\)' "$SOURCE")" -eq 1 ]]
[[ "$(rg -c 'threadPool_->WaitFinish\(\)' "$SOURCE")" -eq 1 ]]

echo 'MSGC EnumRoots uses one persistent GCThreadPool epoch for transition and root publish'
