#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Mutator/MutatorManager.h"
MANAGER="$ROOT_DIR/src/Mutator/MutatorManager.cpp"
MUTATOR_INLINE="$ROOT_DIR/src/Mutator/Mutator.inline.h"
MUTATOR_SOURCE="$ROOT_DIR/src/Mutator/Mutator.cpp"

rg -q 'void NotifyMutatorEnteredSaferegion\(\)' "$HEADER"
rg -q 'std::condition_variable mutatorArrivalCV' "$HEADER"
rg -q 'std::mutex mutatorArrivalMtx' "$HEADER"

rg -q 'STW_MUTATOR_ARRIVAL_SPIN_US = 20' "$MANAGER"
rg -q 'mutatorArrivalCV\.wait_until' "$MANAGER"

WAIT_BODY="$(sed -n \
    '/void MutatorManager::WaitUntilAllMutatorStopped/,/void MutatorManager::EnsurePhaseTransition/p' \
    "$MANAGER")"
if rg -q 'sched_yield\(' <<<"$WAIT_BODY"; then
    echo 'WaitUntilAllMutatorStopped still yields the GC thread to the OS scheduler' >&2
    exit 1
fi

ENTER_BODY="$(sed -n \
    '/inline void Mutator::DoEnterSaferegion/,/inline bool Mutator::EnterSaferegion/p' \
    "$MUTATOR_INLINE")"
rg -q 'SetInSaferegion\(SAFE_REGION_TRUE\)' <<<"$ENTER_BODY"
rg -q 'SyncTriggered\(\)' <<<"$ENTER_BODY"
rg -q 'NotifyMutatorEnteredSaferegion\(\)' <<<"$ENTER_BODY"

publish_line="$(rg -n 'SetInSaferegion\(SAFE_REGION_TRUE\)' <<<"$ENTER_BODY" | cut -d: -f1)"
notify_line="$(rg -n 'NotifyMutatorEnteredSaferegion\(\)' <<<"$ENTER_BODY" | cut -d: -f1)"
if (( publish_line >= notify_line )); then
    echo 'mutator arrival notification must follow saferegion publication' >&2
    exit 1
fi

SUSPEND_BODY="$(sed -n \
    '/void Mutator::HandleSuspensionRequest/,/void Mutator::SuspendForSync/p' \
    "$MUTATOR_SOURCE")"
rg -q 'DoEnterSaferegion\(\)' <<<"$SUSPEND_BODY"
if rg -q 'SetInSaferegion\(SAFE_REGION_TRUE\)' <<<"$SUSPEND_BODY"; then
    echo 'HandleSuspensionRequest bypasses the mutator-arrival notification' >&2
    exit 1
fi
if rg -q 'SetInSaferegion\([^)]*SAFE_REGION_TRUE' \
    "$ROOT_DIR/src/Mutator/Mutator.h" "$MUTATOR_SOURCE"; then
    echo 'a saferegion entry path bypasses DoEnterSaferegion and its arrival notification' >&2
    exit 1
fi

echo 'MSGC STW waits on independently notified mutator arrival after a bounded spin'
