#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
THRESHOLD="$ROOT_DIR/src/Heap/MCollector/MSHeapThresholdPolicy.h"
ADAPTIVE="$ROOT_DIR/src/Heap/MCollector/MSAdaptiveHeapGrowthPolicy.h"
SCAVENGE="$ROOT_DIR/src/Heap/MCollector/MSScavengeWatermarkPolicy.h"
PRESSURE="$ROOT_DIR/src/Heap/MAllocator/MSSweepPressurePolicy.h"
PACING="$ROOT_DIR/src/Heap/MCollector/MSAssistPacing.h"

for header in "$ADAPTIVE" "$SCAVENGE" "$PRESSURE"; do
    if [[ ! -f "$header" ]]; then
        echo "missing focused MSGC policy header: $header" >&2
        exit 1
    fi
done

for symbol in MSAdaptiveHeapGrowthState EvaluateMSAdaptiveHeapGrowth \
              MSScavengeWatermarkDecision EvaluateMSScavengeWatermarks; do
    if rg -q "$symbol" "$THRESHOLD"; then
        echo "MSHeapThresholdPolicy must not own $symbol" >&2
        exit 1
    fi
done

if rg -q '^enum class SweepPressureMode|^inline SweepPressureMode EvaluateSweepPressure|^inline size_t SweepTierMultiplier' \
    "$PACING"; then
    echo 'MSAssistPacing must not define sweep-pressure policy' >&2
    exit 1
fi

if rg -q 'MSStableLiveDecision|MSHeapThresholdState|EvaluateMSStableLive|ComputeMSHeapThresholdMedian|ShouldTriggerMSMark' \
    "$THRESHOLD"; then
    echo 'inactive stable-live threshold policy remains' >&2
    exit 1
fi

echo 'MSGC policy ownership contract holds'
