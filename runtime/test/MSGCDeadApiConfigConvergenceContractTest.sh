#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG_HEADER="$ROOT_DIR/src/Heap/MCollector/MSGCConfig.h"
RUNTIME_INIT="$ROOT_DIR/src/CangjieRuntime.cpp"
API_INIT="$ROOT_DIR/src/CangjieRuntimeApi.cpp"
SCHEDULER_INIT="$ROOT_DIR/src/CjScheduler.cpp"

for symbol in 'GetScanPlane' 'GetDivMagic' 'MSRegionInfo\* GetRegionInfoAt'; do
    if rg -q "$symbol" "$ROOT_DIR/src/Heap/MAllocator"; then
        echo "obsolete MSGC API remains: $symbol" >&2
        exit 1
    fi
done

if rg -q 'RefreshSweepHelperControl\(CentralRegionPool& pool, RegionSpaceMS& space' \
    "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.h" \
    "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"; then
    echo 'RefreshSweepHelperControl still receives unused RegionSpaceMS' >&2
    exit 1
fi

for field in assistBaseRatioPercent assistBacklogStepBuffers assistCreditDecayRounds \
    releasedBusyRetryLimit scavengeRounds reservedBackgroundFeed; do
    if rg -q "$field" "$CONFIG_HEADER"; then
        echo "inactive MSGC resolved-config field remains: $field" >&2
        exit 1
    fi
done

if rg -q 'msConfig\.(assistBaseRatioPercent|assistBacklogStepBuffers|assistCreditDecayRounds|releasedBusyRetryLimit|scavengeRounds|reservedBackgroundFeed)' \
    "$API_INIT" "$SCHEDULER_INIT"; then
    echo 'inactive MSGC GCParam mapping remains' >&2
    exit 1
fi

rg -q 'uint8_t enumAssistEnabled = 0;' "$CONFIG_HEADER"
rg -U -q 'configInput\.enumAssistEnabled =\s*ReadBoolParameter\("cjMSEnumAssist", true\)' \
    "$RUNTIME_INIT"
rg -q 'msEnumAssistEnabled = config.enumAssistEnabled != 0;' "$RUNTIME_INIT"

echo 'MSGC dead API and configuration convergence contract: PASS'
