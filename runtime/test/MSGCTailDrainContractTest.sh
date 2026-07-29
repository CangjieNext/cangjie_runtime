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

rg -q 'size_t QuiesceMarkWorkers\(\)' "$HEADER"
rg -Uq '(?s)markWorkerControl_\.BeginQuiesce\(\);.*WaitForNoInFlight\(\);.*markWorkerControl_\.Stop\(\);' "$SOURCE"
rg -Uq '(?s)outstanding == 0.*ReadyEmptyForTermination\(\).*QuiesceMarkWorkers\(\);.*break;' "$SOURCE"
rg -q '\[mark-stw-entry\] branch=%s gcReason=%s outstanding=%llu' "$SOURCE"
rg -Uq '(?s)while \(markScheduler_\.OutstandingForTermination\(\) != 0\).*TryClaim\(.*MSMarkWorkerCaps::GCWorker\(\)' "$SOURCE"
rg -q 'static MSMarkWorkerCaps Assist\(\) \{ return \{true, false\}; \}' "$SCHED"
rg -q 'replacement must be published before' "$SCHED"

if rg -q 'PromotePutToGet|AcquireOneBuffer|AcquireOneFullBuffer|PublishPartialOutput' "$SOURCE"; then
    echo 'tail drain still contains WorkBuf-specific logic' >&2
    exit 1
fi

echo 'MSGC scheduler fixed-point tail contract holds'
