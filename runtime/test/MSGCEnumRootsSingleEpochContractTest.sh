#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
MANAGER="$ROOT_DIR/src/Mutator/MutatorManager.cpp"

enum_body="$(sed -n '/void MarkSweepCollector::EnumRoots/,/^}/p' "$COLLECTOR")"
printf '%s\n' "$enum_body" | rg -q 'TransitionAllMutatorsToGCPhase'
printf '%s\n' "$enum_body" | rg -q 'PublishRootStaging'
if printf '%s\n' "$enum_body" | rg -q 'enumRootsWorkerSession_|ComputeMSEnumRootsHelperTarget'; then
    echo 'pure assist EnumRoots must not own a GC worker-pool epoch' >&2
    exit 1
fi

ensure="$(sed -n '/void MutatorManager::EnsurePhaseTransition/,/^}/p' "$MANAGER")"
printf '%s\n' "$ensure" | rg -q 'UpdateEnumAssistRemaining'

echo 'MSGC EnumRoots has no GC worker-pool epoch'
