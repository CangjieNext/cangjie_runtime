#!/usr/bin/env bash
set -euo pipefail

runtime_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src_root="$runtime_root/src"
test_root="$runtime_root/test"

removable_pattern='gcMainCompletedTasks|helperCompletedWU|assistCompletedWU|activeLazySweepCount_|activeAssistSweepCount_|activeHelperSweepCount_|MSMarkWorkerLocalStats|MSMarkAssistStats|markAssistStats_|MergeExitedAssistStats|SnapshotAssistStats|exitedAssistStats_|ScavengeSliceStats'

if rg -n "$removable_pattern" "$src_root" "$test_root" \
    --glob '!MSGCRemovableStatsContractTest.sh' \
    --glob '!MSGCHotPathDiagnosticCountersContractTest.sh' \
    --glob '!MSGCDynamicSweepHelperContractTest.sh' \
    --glob '!**/__pycache__/**'; then
    echo "diagnostic-only MSGC statistics remain in Runtime sources or tests" >&2
    exit 1
fi

rg -q 'cycleCompletedMarkWU_' "$src_root/Heap/MCollector/MarkSweepCollector.h"
if rg -q 'cycleAllocatedMarkBytes_|AccountMarkAllocationBytes' "$src_root" "$test_root" \
    --glob '!MSGCRemovableStatsContractTest.sh'; then
    echo "cycleAllocatedMarkBytes must not remain after switching to mark-boundary accounting" >&2
    exit 1
fi
rg -q 'sweepAdmission_' "$src_root/Heap/MAllocator/MSCentralRegionPool.h"
rg -q 'reusedScavengedRegionCount_' "$src_root/Heap/MAllocator/MSFreeManager.h"
