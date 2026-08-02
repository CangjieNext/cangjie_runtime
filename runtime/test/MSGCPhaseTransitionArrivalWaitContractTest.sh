#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Mutator/MutatorManager.h"
MANAGER="$ROOT_DIR/src/Mutator/MutatorManager.cpp"
MUTATOR_INLINE="$ROOT_DIR/src/Mutator/Mutator.inline.h"

rg -q 'void NotifyPhaseTransitionProgress\(\)' "$HEADER"
rg -q 'std::mutex phaseTransitionArrivalMtx' "$HEADER"
rg -q 'std::condition_variable phaseTransitionArrivalCV' "$HEADER"

rg -q 'GC_PHASE_TRANSITION_ARRIVAL_SPIN_US = 20' "$MANAGER"
rg -q 'phaseTransitionArrivalCV\.wait' "$MANAGER"

ENSURE_BODY="$(sed -n \
    '/void MutatorManager::EnsurePhaseTransition/,/void MutatorManager::TransitionAllMutatorsToGCPhase/p' \
    "$MANAGER")"
rg -q 'unique_lock<std::mutex>.*phaseTransitionArrivalMtx' <<<"$ENSURE_BODY"
rg -q 'InSaferegion\(\)' <<<"$ENSURE_BODY"
if rg -q 'sched_yield\(' <<<"$ENSURE_BODY"; then
    echo 'EnsurePhaseTransition still yields the GC thread to the OS scheduler' >&2
    exit 1
fi

TRANSITION_BODY="$(sed -n \
    '/inline bool Mutator::TransitionGCPhase/,/inline bool Mutator::TransitionToCpuProfile/p' \
    "$MUTATOR_INLINE")"
rg -q 'transitionState\.store\(FINISH_TRANSITION, std::memory_order_release\)' <<<"$TRANSITION_BODY"
rg -q 'NotifyPhaseTransitionProgress\(\)' <<<"$TRANSITION_BODY"

publish_line="$(rg -n 'transitionState\.store\(FINISH_TRANSITION' <<<"$TRANSITION_BODY" | cut -d: -f1)"
notify_line="$(rg -n 'NotifyPhaseTransitionProgress\(\)' <<<"$TRANSITION_BODY" | cut -d: -f1)"
if (( publish_line >= notify_line )); then
    echo 'phase transition notification must follow FINISH_TRANSITION publication' >&2
    exit 1
fi

SAFE_BODY="$(sed -n \
    '/inline void Mutator::DoEnterSaferegion/,/inline bool Mutator::EnterSaferegion/p' \
    "$MUTATOR_INLINE")"
rg -q 'HasSuspensionRequest\(SUSPENSION_FOR_GC_PHASE\)' <<<"$SAFE_BODY"
rg -q 'NotifyPhaseTransitionProgress\(\)' <<<"$SAFE_BODY"

echo 'MSGC phase transition waits on independently notified arrivals after a bounded spin'
