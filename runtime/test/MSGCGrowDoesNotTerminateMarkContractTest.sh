#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REGION="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
POLICY="$ROOT_DIR/src/Heap/MCollector/MSMarkTerminationPolicy.h"

for forbidden in 'RequestActiveMarkTerminationBeforeGrow' 'requestedGrowRegions_' \
    'largeGrowRequested_' 'concurrentMarkRunning_' 'markGrowRequestRegions' \
    'SMALL_NEEDS_GROW' 'LARGE_NEEDS_GROW' 'ResolveMSMarkGrowTerminationReason' \
    'small-needs-grow' 'large-needs-grow'; do
    if rg -q "$forbidden" "$REGION" "$HEADER" "$SOURCE" "$POLICY"; then
        echo "heap growth still terminates marking: $forbidden" >&2
        exit 1
    fi
done

rg -q 'void MarkSweepCollector::RunSchedulerMarkLoop\(\)' "$SOURCE"
rg -q 'markScheduler_\.OutstandingHint\(\)' "$SOURCE"

echo 'MSGC heap growth is independent from scheduler convergence'
