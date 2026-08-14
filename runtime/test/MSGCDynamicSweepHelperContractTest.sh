#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CENTRAL_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
CENTRAL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
BACKGROUND_HEADER="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.h"
BACKGROUND_SOURCE="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"
REGION_MANAGER_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
MS_COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
CANGJIE_HEADER="$ROOT_DIR/src/Cangjie.h"
SCHEDULER_SOURCE="$ROOT_DIR/src/CjScheduler.cpp"
RUNTIME_API_SOURCE="$ROOT_DIR/src/CangjieRuntimeApi.cpp"

if ! rg -q 'MSSweepEpochCoordinator sweepEpoch_' "$CENTRAL_HEADER" ||
   rg -q 'activeHelperSweepCount_|helperSweptRegions' "$CENTRAL_HEADER"; then
    echo "parallel Sweep must use unified admission without REPORT-only helper counters" >&2
    exit 1
fi

if ! rg -q 'void ParallelSweep\(' "$CENTRAL_HEADER"; then
    echo "CentralRegionPool must expose a bounded parallel Sweep slice" >&2
    exit 1
fi

if ! rg -q 'forceHighPressure' "$CENTRAL_HEADER" ||
   ! rg -q 'void ForceHighSweepPressure\(\)' "$CENTRAL_HEADER"; then
    echo "CentralRegionPool must own the forced-HIGH Sweep epoch latch" >&2
    exit 1
fi

publish_body="$(awk '
    /void CentralRegionPool::PublishSweepDoneAfterQuiescence\(\)/ { capture = 1 }
    capture { print }
    capture && /^}/ { exit }
' "$CENTRAL_SOURCE")"
if ! printf '%s\n' "$publish_body" | rg -q 'forceHighPressure\.store\(false' ||
   ! printf '%s\n' "$publish_body" | rg -q 'SweepPressureMode::LOW'; then
    echo "only global Sweep completion may clear forced HIGH and publish LOW" >&2
    exit 1
fi
publish_done_line="$(printf '%s\n' "$publish_body" | rg -n 'PublishSweepDoneAfterQuiescence\(\)' | sed -n '1s/:.*//p')"
clear_force_line="$(printf '%s\n' "$publish_body" | rg -n 'forceHighPressure\.store\(false' | sed -n '1s/:.*//p')"
if [[ -z "$publish_done_line" || -z "$clear_force_line" ||
      "$publish_done_line" -ge "$clear_force_line" ]]; then
    echo "Sweep completion must be published before clearing forced HIGH to close the notifier race" >&2
    exit 1
fi

if ! rg -Uq 'void CentralRegionPool::StopAndWaitForAllSweep\(\)[[:space:][:print:]]*CloseSweepAdmissionAndWait\(\)' \
    "$CENTRAL_SOURCE"; then
    echo "PreEnum stop-and-wait must close and wait for all admitted Sweep sources" >&2
    exit 1
fi

if ! rg -Uq 'void CentralRegionPool::ReenableSweepAdmission\(\)[[:space:][:print:]]*skipHelperSweep_' \
    "$CENTRAL_SOURCE"; then
    echo "helper Sweep must be re-enabled only with the existing post-mark Sweep gates" >&2
    exit 1
fi

force_finish_body="$(awk '
    /void CentralRegionPool::ForceFinishAllUnswept\(\)/ { capture = 1 }
    capture { print }
    capture && /^}/ { exit }
' "$CENTRAL_SOURCE")"
if printf '%s\n' "$force_finish_body" | rg -q 'sweepDone_\.store'; then
    echo "ForceFinishAllUnswept must not publish sweepDone before global quiescence" >&2
    exit 1
fi

stop_wait_body="$(awk '
    /void CentralRegionPool::StopAndWaitForAllSweep\(\)/ { capture = 1 }
    capture { print }
    capture && /^}/ { exit }
' "$CENTRAL_SOURCE")"
force_finish_count="$(printf '%s\n' "$stop_wait_body" | rg -c 'ForceFinishAllUnswept\(\);')"
first_force_line="$(printf '%s\n' "$stop_wait_body" | rg -n 'ForceFinishAllUnswept\(\);' | sed -n '1s/:.*//p')"
stop_line="$(printf '%s\n' "$stop_wait_body" | rg -n 'stopSweepRequested_\.store\(true' | sed -n '1s/:.*//p')"
wait_line="$(printf '%s\n' "$stop_wait_body" | rg -n 'CloseSweepAdmissionAndWait\(\)' | sed -n '1s/:.*//p')"
second_force_line="$(printf '%s\n' "$stop_wait_body" | rg -n 'ForceFinishAllUnswept\(\);' | sed -n '2s/:.*//p')"
publish_line="$(printf '%s\n' "$stop_wait_body" | rg -n 'PublishSweepDoneAfterQuiescence\(\)' | sed -n '1s/:.*//p')"
if [[ "$force_finish_count" -ne 2 || -z "$first_force_line" || -z "$stop_line" ||
      -z "$wait_line" || -z "$second_force_line" || -z "$publish_line" ||
      "$first_force_line" -ge "$stop_line" || "$stop_line" -ge "$wait_line" ||
      "$wait_line" -ge "$second_force_line" || "$second_force_line" -ge "$publish_line" ]]; then
    echo "PreEnum closing Sweep must cooperative-drain, close admission, wait, final-drain, then publish" >&2
    exit 1
fi

python3 - "$CENTRAL_SOURCE" "$BACKGROUND_SOURCE" <<'PY'
import pathlib
import sys

central = pathlib.Path(sys.argv[1]).read_text()
background = pathlib.Path(sys.argv[2]).read_text()

def section(source, start_token, end_token):
    start = source.index(start_token)
    end = source.index(end_token, start)
    return source[start:end]

background_sweep = section(
    central,
    'void CentralRegionPool::BackgroundSweep(',
    'void CentralRegionPool::ParallelSweep(',
)
parallel_sweep = section(
    central,
    'void CentralRegionPool::ParallelSweep(',
    'size_t CentralRegionPool::AssistSweep(',
)
assist_sweep = section(
    central,
    'size_t CentralRegionPool::AssistSweep(',
    'bool CentralRegionPool::SweepOneUnsweptRegionForAssist(',
)
sweep_list = section(
    central,
    'bool CentralRegionPool::SweepUnsweptList(',
    '// ===== Monitoring =====',
)

if background_sweep.count('stopSweepRequested_.load') != 1:
    raise SystemExit('Background Sweep must check closure only once at slice admission')
if parallel_sweep.count('stopSweepRequested_.load') != 1:
    raise SystemExit('Parallel Helper must check closure only once at slice admission')
if assist_sweep.count('stopSweepRequested_.load') != 1:
    raise SystemExit('Assist must check closure once at admission, not for each Region')
if 'SweepOneUnsweptRegionForAssist' not in assist_sweep:
    raise SystemExit('Assist must claim from the unified unswept head')
if 'stopSweepRequested_.load' in sweep_list:
    raise SystemExit('bounded Background/Helper slices must not recheck closure for each Region')
if 'std::this_thread::yield()' in sweep_list:
    raise SystemExit('bounded Background/Helper slices must not yield inside the Region loop')
if 'TimeUtil::MicroSeconds() >= deadlineUs' not in sweep_list:
    raise SystemExit('Background/Helper slices must retain their deadline boundary')

stop_wait = section(
    central,
    'void CentralRegionPool::StopAndWaitForAllSweep()',
    'void CentralRegionPool::VisitMarkPendingRegionsUnderSTW(',
)
final_drain = stop_wait.rfind('ForceFinishAllUnswept();')
exact_check = stop_wait.find('CHECK(GetTotalUnsweptCountReference() == 0)', final_drain)
publish_check = stop_wait.find('PublishSweepDoneAfterQuiescence()', final_drain)
if final_drain < 0 or exact_check < final_drain or publish_check < exact_check:
    raise SystemExit('PreEnum closure must final-drain, verify exact emptiness, then require completion publication')

run_loop = section(
    background,
    'void BackgroundProcessor::Run()',
    'void BackgroundProcessor::Init()',
)
immediate = run_loop.find('bool continueParallelSweep =')
generic_backoff = run_loop.find('if (shouldContinueImmediate)')
if immediate < 0 or generic_backoff < 0 or immediate > generic_backoff:
    raise SystemExit('continued Parallel Sweep pressure must bypass the generic yield/backoff path')
PY

if ! rg -q 'NotifyToParallelSweep\(\)' "$REGION_MANAGER_SOURCE" ||
   rg -q 'NotifyToParallelSweep|EvaluateMSHeuExecution' "$MS_COLLECTOR_SOURCE"; then
    echo "only HEU submission pressure may wake dynamic parallel Sweep" >&2
    exit 1
fi

if ! rg -q 'shouldParallelSweep_' "$BACKGROUND_HEADER" ||
   ! rg -q 'activeSweepHelperCount_' "$BACKGROUND_HEADER" ||
   ! rg -q 'MSGCWorkerPoolSession sweepSession' "$BACKGROUND_SOURCE" ||
   rg -q 'TryRequestAsyncGC\(GC_REASON_HEU\)|DeferredHeuOrigin|deferredHeu' \
       "$BACKGROUND_HEADER" "$BACKGROUND_SOURCE"; then
    echo "BackgroundProcessor must coordinate only SWEEP lease work, without HEU retry state" >&2
    exit 1
fi

if rg -q 'targetSweepHelperCount_|leaseMiss' "$BACKGROUND_HEADER" "$BACKGROUND_SOURCE"; then
    echo "BackgroundProcessor must retain only scheduling state that affects Sweep execution" >&2
    exit 1
fi

if rg -q 'ComputeMSParallelSweepPlan|regionQuantum|msAssistSweepMaxRegionsPerCall' "$BACKGROUND_SOURCE" ||
   ! rg -q 'ComputeMSSweepTargetHelpers|ComputeMSActiveSweepHelpers' "$BACKGROUND_SOURCE" ||
   ! rg -q 'ParallelSweep\([^,]+,[^,]+\)' "$BACKGROUND_SOURCE" ||
   ! rg -Uq 'ParallelSweep\([^;]+deadlineUs\)' "$BACKGROUND_SOURCE"; then
    echo "Helper Sweep must use pressure/time control, not a Region quantum or P batch cap" >&2
    exit 1
fi

notify_body="$(awk '
    /bool BackgroundProcessor::NotifyToParallelSweep\(\)/ { capture = 1 }
    capture { print }
    capture && /^}/ { exit }
' "$BACKGROUND_SOURCE")"
force_line="$(printf '%s\n' "$notify_body" | rg -n 'ForceHighSweepPressure' | sed -n '1s/:.*//p')"
wake_line="$(printf '%s\n' "$notify_body" | rg -n 'RequestWork\(shouldParallelSweep_\)' | sed -n '1s/:.*//p')"
if [[ -z "$force_line" || -z "$wake_line" || "$force_line" -ge "$wake_line" ]]; then
    echo "NotifyToParallelSweep must force HIGH before waking the coordinator" >&2
    exit 1
fi

if ! rg -Uq 'AcquireMSGCWorkerPool\([[:space:]]*MSGCWorkerPoolOwner::MARK,[[:space:]]*true\)' "$MS_COLLECTOR_SOURCE"; then
    echo "concurrent Mark helpers must own the shared worker pool" >&2
    exit 1
fi

for config_file in "$CANGJIE_HEADER" "$SCHEDULER_SOURCE" "$RUNTIME_API_SOURCE"; do
    if ! rg -q 'msParallelSweep' "$config_file"; then
        echo "dynamic parallel Sweep configuration missing: $config_file" >&2
        exit 1
    fi
done

echo "MSGC dynamic Sweep helper Region contract holds"
