#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Mutator/MutatorManager.h"
SOURCE="$ROOT_DIR/src/Mutator/MutatorManager.cpp"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

rg -q 'struct STWAcquireStats' "$HEADER"
for field in \
    syncMutexWaitUs \
    mutatorLockWaitUs \
    mutatorCountUs \
    suspensionBroadcastUs \
    unstoppedSnapshotUs \
    safepointWaitUs \
    phaseTransitionUs \
    mutatorCount \
    initialUnstoppedMutators; do
    rg -q "$field" "$HEADER"
done

rg -q 'STWAcquireStats\* stats' "$HEADER"
for field in \
    syncMutexWaitUs \
    mutatorLockWaitUs \
    mutatorCountUs \
    suspensionBroadcastUs \
    unstoppedSnapshotUs \
    safepointWaitUs \
    phaseTransitionUs; do
    rg -q "stats->$field" "$SOURCE"
done
rg -q 'initialUnstoppedMutators' "$SOURCE"

rg -q 'STWAcquireStats stwAcquireStats' "$COLLECTOR"
rg -q 'syncMutexWait=%sus mutatorLockWait=%sus mutatorCount=%sus' "$COLLECTOR"
rg -q 'suspensionBroadcast=%sus unstoppedSnapshot=%sus' "$COLLECTOR"
rg -q 'safepointWait=%sus phaseTransition=%sus stwAcquireOther=%sus' "$COLLECTOR"
rg -q 'mutators=%zu initialUnstopped=%zu' "$COLLECTOR"

echo 'MSGC MarkTermination stwAcquire exposes every acquisition substage'
