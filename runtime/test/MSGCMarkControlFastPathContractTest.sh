#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
SCHED_HEADER="$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.h"
SCHED_SOURCE="$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.cpp"

rg -q 'MSMarkScheduler markScheduler_;' "$HEADER"
rg -q 'WorkEpoch markWorkEpoch_;' "$HEADER"
rg -q 'MSMarkWorkerControl markWorkerControl_\{&markWorkEpoch_\};' "$HEADER"
rg -q 'RunMarkSlice\(' "$SCHED_HEADER"
rg -q 'MSMarkWorkerCaps::GCWorker\(\)' "$SOURCE"
rg -q 'RunMarkAssistSlice\(' "$SOURCE"
CONTROL_HEADER="$ROOT_DIR/src/Heap/MCollector/MSMarkWorkerControl.h"
WORK_UNIT_HEADER="$ROOT_DIR/src/Heap/MCollector/MSWorkUnit.h"
rg -q 'bool TryEnterAssist\(\)' "$CONTROL_HEADER"
assist_entry="$(sed -n '/bool TryEnterAssist()/,/^    }/p' "$CONTROL_HEADER")"
if printf '%s\n' "$assist_entry" |
    rg -q 'markThreadCapacity|workersInFlight_|ShouldAdmitMSMarkAssist'; then
    echo 'Assist admission still applies a machine-core limit' >&2
    exit 1
fi
if rg -q 'ShouldAdmitMSMarkAssist' "$WORK_UNIT_HEADER"; then
    echo 'obsolete Assist core-limit policy remains' >&2
    exit 1
fi
mark_assist="$(sed -n '/size_t MarkSweepCollector::MSMarkAssist/,/^}/p' "$SOURCE")"
if printf '%s\n' "$mark_assist" | rg -q 'GetMSGCThreadCapacity'; then
    echo 'MSMarkAssist still reads machine-core capacity' >&2
    exit 1
fi
rg -q 'gcAssistCreditWU_' "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
rg -q 'gcAssistProbeCursor_' "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
rg -q 'markWorkEpoch_\.WaitForChange' "$SOURCE"
rg -Uq '(?s)outstanding == 0.*workersInFlight == 0.*assistsInFlight == 0.*ReadyEmptyForTermination\(\)' "$SOURCE"

publish="$(sed -n '/bool MSMarkScheduler::TryPublishRegion/,/^}/p' "$SCHED_SOURCE")"
idle_fast_reject="$(printf '%s\n' "$publish" |
    rg -n 'GetScanState\(\) != MSScanState::IDLE' |
    cut -d: -f1)"
provisional="$(printf '%s\n' "$publish" | rg -n 'ledger_\.AddProvisional' | cut -d: -f1)"
cas="$(printf '%s\n' "$publish" |
    rg -n 'CasScanState\(MSScanState::IDLE, MSScanState::QUEUED\)' |
    cut -d: -f1)"
push="$(printf '%s\n' "$publish" | rg -n 'RegionReadyShard\(preferredRegionShard\)\.Push' | cut -d: -f1)"
if [[ -z "$idle_fast_reject" || -z "$provisional" || -z "$cas" || -z "$push" ||
      "$idle_fast_reject" -ge "$provisional" ||
      "$provisional" -ge "$cas" || "$cas" -ge "$push" ]]; then
    echo 'TaskLedger admission must precede Region publication' >&2
    exit 1
fi

if rg -q 'globalFull_|workBufPool_|PerPGCWork|activeLog_' "$HEADER" "$SOURCE"; then
    echo 'ordinary MSGC WorkBuf scheduling remains in the collector' >&2
    exit 1
fi

echo 'MSGC unified scheduler/control contract holds'
