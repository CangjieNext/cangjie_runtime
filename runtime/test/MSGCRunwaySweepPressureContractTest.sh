#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
POOL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
BACKGROUND_SOURCE="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"

pool_text="$(tr '\n' ' ' < "$POOL_SOURCE")"

if ! rg -q 'SweepPressureMode previousMode = GetSweepPressureMode\(\);' "$POOL_SOURCE"; then
    echo "RefreshSweepDebtSnapshot must load the previous published pressure mode" >&2
    exit 1
fi

if ! printf '%s\n' "$pool_text" |
    rg -q 'EvaluateSweepPressure\([^;]*forcedHigh,[[:space:]]*previousMode\)'; then
    echo "RefreshSweepDebtSnapshot must pass previousMode to EvaluateSweepPressure" >&2
    exit 1
fi

if ! rg -q 'size_t runwayBytes = ComputeMSSweepRunwayBytes\(thresholdBytes, sweptBytes\);' \
    "$BACKGROUND_SOURCE"; then
    echo "parallel Sweep must compute remaining runway for REPORT logging" >&2
    exit 1
fi

if ! rg -q 'U=%zuB S=%zuB T=%zuB R=%zuB' "$BACKGROUND_SOURCE"; then
    echo "parallel-sweep start REPORT must expose U/S/T/R" >&2
    exit 1
fi

echo "MSGC runway Sweep pressure contract: PASS"
