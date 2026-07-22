#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
CONTROL_HEADER="$ROOT_DIR/src/Heap/MCollector/MSMarkWorkerControl.h"

if rg -n 'restartOnLateWork|resumeHelpers|markWorkerControl_\.Resume|void Resume\(' \
    "$COLLECTOR_HEADER" "$COLLECTOR_SOURCE" "$CONTROL_HEADER"; then
    echo "MSGC concurrent mark still has a restartable quiesce path" >&2
    exit 1
fi

if ! rg -q 'size_t QuiesceMarkWorkers\(\)' "$COLLECTOR_HEADER"; then
    echo "MSGC quiesce must be a parameterless one-way transition" >&2
    exit 1
fi

if ! rg -q 'workerPolicy\.EnterTail\(\)' "$COLLECTOR_SOURCE"; then
    echo "MSGC must pin all helpers after entering tail drain" >&2
    exit 1
fi

if ! rg -q 'ComputeMSAssistTailRatioScaled' "$COLLECTOR_SOURCE"; then
    echo "MSGC tail drain must raise assist to its configured maximum multiplier" >&2
    exit 1
fi

if rg -n 'CheckConvergenceAndBreak|kConvergenceRequiredRounds|kConvergenceYieldUs|convergenceStableRounds_' \
    "$COLLECTOR_HEADER" "$COLLECTOR_SOURCE"; then
    echo "MSGC one-way quiesce must not wait for repeated empty rounds" >&2
    exit 1
fi

if ! rg -Uq 'if \(convergenceCandidate\) \{[[:space:][:print:]]*quiesceResidualBuffers = QuiesceMarkWorkers\(\);[[:space:]]*break;' \
    "$COLLECTOR_SOURCE"; then
    echo "MSGC must quiesce immediately on the first complete empty candidate" >&2
    exit 1
fi

echo "MSGC tail drain and one-way quiesce contract holds"
