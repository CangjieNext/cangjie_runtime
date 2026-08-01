#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Mutator/MutatorManager.h"
SOURCE="$ROOT_DIR/src/Mutator/MutatorManager.cpp"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

rg -q 'struct EnumAssistContext' "$HEADER"
rg -q 'std::vector<Mutator\*> candidates' "$HEADER"
rg -q 'size_t claimCursor = 0' "$HEADER"
rg -q 'size_t inFlight = 0' "$HEADER"
rg -q 'std::condition_variable drained' "$HEADER"
if rg -q 'EnumTransitionEntry|EnumTransitionState|MSEnumRootsWorkerSession' "$HEADER"; then
    echo 'EnumRoots must not retain the per-entry/GC-helper transition model' >&2
    exit 1
fi

claim="$(sed -n '/size_t MutatorManager::ClaimEnumAssistBatch/,/^}/p' "$SOURCE")"
printf '%s\n' "$claim" | rg -q 'std::lock_guard<std::mutex>'
printf '%s\n' "$claim" | rg -q 'claimCursor\+\+'
printf '%s\n' "$claim" | rg -q 'claimCursor < .*candidates\.size'
if printf '%s\n' "$claim" | rg -q 'compare_exchange|fetch_add|probeLimit'; then
    echo 'pure Enum assist must use one monotonic batch cursor, not CAS probing' >&2
    exit 1
fi

assist="$(sed -n '/size_t MutatorManager::AssistEnumRoots/,/^}/p' "$SOURCE")"
printf '%s\n' "$assist" | rg -q 'ClaimEnumAssistBatch'
printf '%s\n' "$assist" | rg -q 'candidate->TransitionGCPhase\(false\)'
printf '%s\n' "$assist" | rg -q 'FinishEnumAssistBatch'

ensure="$(sed -n '/void MutatorManager::EnsurePhaseTransition/,/^}/p' "$SOURCE")"
printf '%s\n' "$ensure" | rg -q 'UpdateEnumAssistRemaining'
printf '%s\n' "$ensure" | rg -q 'undoneMutators'

enum_body="$(sed -n '/void MarkSweepCollector::EnumRoots/,/^}/p' "$COLLECTOR")"
if printf '%s\n' "$enum_body" | rg -q 'enumRootsWorkerSession_|ComputeMSEnumRootsHelperTarget'; then
    echo 'EnumRoots must not start or wait for GC helpers' >&2
    exit 1
fi

publish="$(sed -n '/void MarkSweepCollector::PublishRootStaging/,/^}/p' "$COLLECTOR")"
printf '%s\n' "$publish" | rg -q 'PublishRootRange\(roots, 0, uniqueRootCount\)'
if printf '%s\n' "$publish" | rg -q 'RunStage|WaitStage|enumRootsWorkerSession_'; then
    echo 'root publication must be serial in the pure-assist model' >&2
    exit 1
fi

echo 'MSGC EnumRoots uses GC-main convergence plus monotonic mutator assist only'
