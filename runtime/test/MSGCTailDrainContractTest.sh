#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
CONTROL="$ROOT_DIR/src/Heap/MCollector/MSMarkWorkerControl.h"
SCHED="$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.h"

if rg -n 'restartOnLateWork|resumeHelpers|markWorkerControl_\.Resume|void Resume\(' \
    "$HEADER" "$SOURCE" "$CONTROL"; then
    echo 'MSGC quiesce must remain a one-way transition' >&2
    exit 1
fi

rg -q 'void QuiesceMarkWorkers\(\)' "$HEADER"
rg -Uq '(?s)markWorkerControl_\.BeginQuiesce\(\);.*WaitForNoInFlight\(\);.*markWorkerControl_\.Stop\(\);' "$SOURCE"
rg -q 'markWorkEpoch_\.WaitForChange' "$SOURCE"
rg -Uq '(?s)outstanding == 0.*workersInFlight == 0.*assistsInFlight == 0.*ReadyEmptyForTermination\(\).*QuiesceMarkWorkers\(\);.*break;' "$SOURCE"
rg -Uq '(?s)PublishOrdinaryMarkTasksUnderSTW\(\*centralPool\).*DrainSchedulerWithHelpersUnderSTW\(\).*PublishFinalizerResurrectionRootsUnderSTW.*DrainSchedulerWithHelpersUnderSTW\(\)' "$SOURCE"
rg -Uq '(?s)while \(markScheduler_\.OutstandingForTermination\(\) != 0\).*TryClaim\(.*true,' "$SOURCE"
rg -q 'bool TryClaimAssist\(' "$SCHED"

if rg -n 'DRAINING_VERIFY|BeginDrain\(|WaitForNoAssists\(|PublicationEpoch\(|stableEmptyEpoch|kMSMarkDrainStableUs' \
    "$SOURCE" "$CONTROL" "$SCHED"; then
    echo 'CurrentMark still contains draining-verify cutoff optimization' >&2
    exit 1
fi

if rg -q 'PromotePutToGet|AcquireOneBuffer|AcquireOneFullBuffer|PublishPartialOutput' "$SOURCE"; then
    echo 'tail drain still contains WorkBuf-specific logic' >&2
    exit 1
fi

echo 'MSGC immediate quiesce tail contract holds'
