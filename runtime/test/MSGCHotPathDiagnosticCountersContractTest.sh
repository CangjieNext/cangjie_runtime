#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ALLOC_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
ALLOC_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
SWEEP_POLICY="$ROOT_DIR/src/Heap/MAllocator/MSSweepDebtPolicy.h"
CENTRAL_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
CENTRAL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
FREE_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.h"
FREE_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.cpp"
ALLOCATOR_HEADER="$ROOT_DIR/src/Heap/Allocator/Allocator.h"
BACKGROUND_HEADER="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.h"
BACKGROUND_SOURCE="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"
MARK_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
MARK_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
WB_STATS_HEADER="$ROOT_DIR/src/Heap/MCollector/MSGCWriteBarrierFlushStats.h"
RUNTIME_STATS_HEADER="$ROOT_DIR/src/RuntimeStats.h"

if rg -q 'MSSweepAssistAtomicStats|MSSweepAssistStats|sweepAssistStats_|SnapshotSweepAssistStats|exitedSweepAssistStats_' \
    "$SWEEP_POLICY" "$ALLOC_HEADER" "$ALLOC_SOURCE" "$CENTRAL_HEADER" "$CENTRAL_SOURCE"; then
    echo "P-level Sweep diagnostic statistics must not remain in allocation paths" >&2
    exit 1
fi

if ! rg -q 'uint32_t sweepDebtGen_' "$ALLOC_HEADER" ||
   rg -q 'std::atomic<uint32_t> sweepDebtGen_' "$ALLOC_HEADER"; then
    echo "Sweep debt generation must remain as P-local non-atomic state" >&2
    exit 1
fi

if rg -q 'MSSweepCounts|backgroundSweptRegions_|lazySweptRegions_|assistSweptRegions_|helperSweptRegions_' \
    "$CENTRAL_HEADER" "$CENTRAL_SOURCE"; then
    echo "Central Sweep source counters are diagnostic-only and must be removed" >&2
    exit 1
fi

if ! rg -q 'sweepCredit_\.Add' "$CENTRAL_HEADER" ||
   ! rg -q 'sweepAdmission_' "$CENTRAL_HEADER"; then
    echo "functional Sweep credit and unified admission state must remain" >&2
    exit 1
fi

if rg -q 'snapFreeRegions_|snapScavengedRegions_' "$FREE_HEADER" "$FREE_SOURCE" ||
   ! rg -q 'snapHotFreeRegions_' "$FREE_HEADER" "$FREE_SOURCE"; then
    echo "FreeManager must retain only the functional hot-free lockless snapshot" >&2
    exit 1
fi

if [[ -e "$WB_STATS_HEADER" ]] ||
   rg -q 'MSGCWriteBarrierFlushStats|wbFlushStats_|wbFlushSnapshot|WriteBarrierFlush:' "$MARK_HEADER" "$MARK_SOURCE"; then
    echo "write-barrier flush counters and their REPORT-only plumbing must be removed" >&2
    exit 1
fi

if [[ -e "$ROOT_DIR/src/Heap/MCollector/MSMarkLocalStats.h" ]] ||
   rg -q 'MSMarkAssistStats|MSMarkWorkerLocalStats|markAssistStats_' \
       "$MARK_HEADER" "$MARK_SOURCE" "$ALLOC_HEADER"; then
    echo "mark worker and assist REPORT-only statistics must be removed" >&2
    exit 1
fi

if rg -q 'peakActiveHelpers_|workerScaleUpCount_|peakPublishedBacklog_|bgCreditInjected_|markTermProcessedObjects' \
    "$MARK_HEADER" "$MARK_SOURCE"; then
    echo "mark-loop and mark-termination REPORT-only counters must be removed" >&2
    exit 1
fi

if rg -q 'BackgroundSweepStats|BackgroundReclaimStats|ParallelSweepTaskContext|pendingSweepStats_|pendingScavengeStats_|pendingShrinkStats_' \
    "$ALLOCATOR_HEADER" "$BACKGROUND_HEADER" "$BACKGROUND_SOURCE" "$CENTRAL_HEADER" "$CENTRAL_SOURCE"; then
    echo "background REPORT-only aggregate statistics must be removed" >&2
    exit 1
fi

if ! rg -q 'void BackgroundSweep\(' "$CENTRAL_HEADER" ||
   ! rg -q 'void ParallelSweep\(' "$CENTRAL_HEADER"; then
    echo "background Sweep operations must remain after dropping their diagnostic return values" >&2
    exit 1
fi

if rg -q 'MSLargeSweepStats|losFreedCount|losCollectUs' "$CENTRAL_HEADER" "$CENTRAL_SOURCE" "$MARK_HEADER" "$MARK_SOURCE" ||
   ! rg -q 'void SweepLargeRegions\(' "$CENTRAL_HEADER"; then
    echo "large Sweep must not build REPORT-only result structures" >&2
    exit 1
fi

if [[ -e "$RUNTIME_STATS_HEADER" ]]; then
    echo "unused RuntimeStats declarations must be removed" >&2
    exit 1
fi

for function_name in DumpGCStateSnapshot DumpRegionStatsMS; do
    if ! rg -Uq "void MarkSweepCollector::${function_name}[^}]+IsMSGCReportEnabled" "$MARK_SOURCE"; then
        echo "${function_name} must short-circuit when MSGC REPORT logging is disabled" >&2
        exit 1
    fi
done

if ! rg -q 'trueLiveBytes' "$MARK_HEADER" ||
   rg -q 'gcMainCompletedTasks|helperCompletedWU|assistCompletedWU' "$MARK_HEADER" "$MARK_SOURCE"; then
    echo "functional live-byte accounting must remain without REPORT-only mark totals" >&2
    exit 1
fi

echo "MSGC hot-path diagnostic counters contract holds"
