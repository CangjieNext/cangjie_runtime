#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.cpp"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

rg -q 'MSTaskLease\(const MSTaskLease&\) = delete' "$HEADER"
rg -q 'operator=\(const MSTaskLease&\) = delete' "$HEADER"
rg -q 'static MSMarkWorkerCaps Assist\(\) \{ return \{true, false\}; \}' "$HEADER"
rg -q 'OutstandingForTermination' "$HEADER"
rg -q 'ReadyEmptyForTermination' "$HEADER"
rg -q 'bool Push\(MSRegionInfo\* region\)' "$HEADER"
rg -q 'class alignas\(64\) MSMarkReadyStack' "$HEADER"
rg -q 'ConfigureRegionShards' "$HEADER"
rg -q 'std::unique_ptr<MSMarkReadyStack\[\]> regionReadyShards_' "$HEADER"
rg -q 'preferredRegionShard' "$HEADER" "$SOURCE"
rg -q 'size_t& regionProbeCursor' "$HEADER" "$SOURCE"
if rg -q 'std::atomic<size_t> regionProbeCursor_' "$HEADER"; then
    echo 'GC workers still share one Region probe cursor' >&2
    exit 1
fi
rg -q 'TryMSMarkReadyShardProbe' "$SOURCE"
rg -q 'TryMSMarkReadyAssistShardProbe' "$SOURCE"
rg -q 'kMSMarkReadyAssistRemoteProbes = 2' "$ROOT_DIR/src/Heap/MCollector/MSMarkReadyShardPolicy.h"
rg -q 'bool fullFallback = ledger_\.LoadHint\(\) != 0' "$SOURCE"
rg -q 'RunMarkAssistSlice' "$HEADER" "$COLLECTOR"

publish="$(sed -n '/bool MSMarkScheduler::TryPublishRegion/,/^}/p' "$SOURCE")"
idle_fast_reject="$(printf '%s\n' "$publish" |
    rg -n 'GetScanState\(\) != MSScanState::IDLE' |
    cut -d: -f1)"
provisional="$(printf '%s\n' "$publish" | rg -n 'ledger_\.AddProvisional' | cut -d: -f1)"
admission_cas="$(printf '%s\n' "$publish" |
    rg -n 'CasScanState\(MSScanState::IDLE, MSScanState::QUEUED\)' |
    cut -d: -f1)"
pending_checks="$(printf '%s\n' "$publish" |
    rg -n 'LoadPendingMarkWordsAcquire' |
    cut -d: -f1)"
first_pending_check="$(printf '%s\n' "$pending_checks" | head -1)"
second_pending_check="$(printf '%s\n' "$pending_checks" | tail -1)"
rollback_cas="$(printf '%s\n' "$publish" |
    rg -n 'CasScanState\(MSScanState::QUEUED, MSScanState::IDLE\)' |
    cut -d: -f1)"
push="$(printf '%s\n' "$publish" | rg -n 'RegionReadyShard\(preferredRegionShard\)\.Push' | cut -d: -f1)"
if [[ -z "$idle_fast_reject" ||
      "$(printf '%s\n' "$pending_checks" | wc -l | tr -d ' ')" -ne 2 ||
      "$idle_fast_reject" -ge "$provisional" ||
      "$provisional" -ge "$admission_cas" ||
      "$admission_cas" -ge "$first_pending_check" ||
      "$first_pending_check" -ge "$push" ||
      "$first_pending_check" -ge "$rollback_cas" ||
      "$rollback_cas" -ge "$second_pending_check" ]]; then
    echo 'Region publication does not safely retract a stale empty admission' >&2
    exit 1
fi

if rg -q 'count_|CountHint' "$HEADER" "$SOURCE"; then
    echo 'ready stack retains diagnostic-only contended counters' >&2
    exit 1
fi

if rg -q 'RequeueOwnedRegion' "$HEADER" "$SOURCE"; then
    echo 'dead finite-budget Region replacement path remains' >&2
    exit 1
fi

region_task="$(sed -n '/void MarkSweepCollector::RunRegionTask/,/^}/p' "$COLLECTOR")"
take_pending_line="$(printf '%s\n' "$region_task" |
    rg -n 'TakePendingMarkWords' | head -1 | cut -d: -f1)"
idle_release_line="$(printf '%s\n' "$region_task" |
    rg -n 'CasScanState\(MSScanState::OWNED, MSScanState::IDLE\)' | cut -d: -f1)"
post_release_pending_line="$(printf '%s\n' "$region_task" |
    rg -n 'LoadPendingMarkWordsAcquire' | tail -1 | cut -d: -f1)"
post_release_publish_line="$(printf '%s\n' "$region_task" |
    rg -n 'markScheduler_\.TryPublishRegion' | tail -1 | cut -d: -f1)"
normal_complete_line="$(printf '%s\n' "$region_task" | rg -n 'lease\.Complete' | tail -1 | cut -d: -f1)"
if [[ -z "$idle_release_line" || -z "$post_release_pending_line" ||
      "$take_pending_line" -ge "$idle_release_line" ||
      "$idle_release_line" -ge "$post_release_pending_line" ||
      "$post_release_pending_line" -ge "$post_release_publish_line" ||
      "$post_release_publish_line" -ge "$normal_complete_line" ]]; then
    echo 'Region owner must republish residual pending work before lease completion' >&2
    exit 1
fi

rg -q 'OutstandingWorkHint' "$HEADER" "$SOURCE"
rg -q 'PublishRegionWords' "$HEADER" "$SOURCE"
rg -q 'CompleteRegionWork' "$HEADER" "$SOURCE"

if rg -q 'CONTROL_MARK_DIRTY|ClearMarkDirtyForOwner|TryReleaseScanOwnerIfClean' \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionInfo.h" "$COLLECTOR" "$SOURCE"; then
    echo 'hot-path Mark DIRTY handshake remains' >&2
    exit 1
fi

termination="$(sed -n '/void MarkSweepCollector::MarkTermination/,/^}/p' "$COLLECTOR")"
drain_line="$(printf '%s\n' "$termination" | rg -n 'DrainSchedulerUnderSTW' | head -1 | cut -d: -f1)"
reconcile_line="$(printf '%s\n' "$termination" |
    rg -n 'VisitMarkPendingRegionsUnderSTW' | cut -d: -f1)"
republish_line="$(printf '%s\n' "$termination" |
    rg -n 'markScheduler_\.TryPublishRegion' | cut -d: -f1)"
if [[ -z "$drain_line" || -z "$reconcile_line" || -z "$republish_line" ||
      "$drain_line" -ge "$reconcile_line" ||
      "$reconcile_line" -ge "$republish_line" ]]; then
    echo 'MarkTermination does not reconcile stranded pending Region work' >&2
    exit 1
fi

object_publish="$(sed -n '/bool MSMarkScheduler::PublishObject/,/^}/p' "$SOURCE")"
provisional="$(printf '%s\n' "$object_publish" | rg -n 'ledger_\.AddProvisional' | cut -d: -f1)"
push="$(printf '%s\n' "$object_publish" | rg -n 'return objectReady_\.Push' | cut -d: -f1)"
if [[ "$provisional" -ge "$push" ]]; then
    echo 'Object task publication must enter the ledger before the ready stack' >&2
    exit 1
fi

echo 'MSGC scheduler ownership and termination-ledger contract holds'
