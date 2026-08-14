#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)

require_absent() {
    local pattern=$1
    shift
    if rg -n -w "$pattern" "$@"; then
        echo "obsolete MSGC symbol remains: $pattern" >&2
        exit 1
    fi
}

if [[ -e "$ROOT_DIR/src/Heap/MAllocator/CMakeLists.txt" ]]; then
    echo 'unreachable MAllocator CMake target remains' >&2
    exit 1
fi

require_absent 'ENUM_ROOTS' \
    "$ROOT_DIR/src/Heap/MCollector/MSGCWorkerPoolController.h" \
    "$ROOT_DIR/src/Heap/Collector/CollectorResources.h" \
    "$ROOT_DIR/src/Heap/Collector/CollectorResources.cpp"
require_absent 'POST_MARK_STATS' "$ROOT_DIR/src/Heap/MCollector/MSGCWorkerPoolController.h"
require_absent 'AcquireEnumRoots' \
    "$ROOT_DIR/src/Heap/MCollector/MSGCWorkerPoolController.h" \
    "$ROOT_DIR/src/Heap/Collector/CollectorResources.h" \
    "$ROOT_DIR/src/Heap/Collector/CollectorResources.cpp"
require_absent 'AcquireMSGCWorkerPoolForEnumRoots' \
    "$ROOT_DIR/src/Heap/Collector/CollectorResources.h" \
    "$ROOT_DIR/src/Heap/Collector/CollectorResources.cpp"

require_absent 'IsLargeRegionEntryType' "$ROOT_DIR/src/Heap/MAllocator/MSRegionInfo.h"
require_absent 'MSAdaptiveHeapGrowthActionName' "$ROOT_DIR/src/Heap/MCollector/MSHeapThresholdPolicy.h"
require_absent 'GetPlaneBase' \
    "$ROOT_DIR/src/Heap/MAllocator/MSBitmapArena.h" \
    "$ROOT_DIR/src/Heap/MAllocator/MSBitmapArena.cpp"
require_absent 'HasEmptySweptRegions' \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.h" \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
require_absent 'ComputeMaxVirtualHeapSize' \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h" \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
require_absent 'ClearMarkBitmap' "$ROOT_DIR/src/Heap/MAllocator/MSRegionInfo.h"
require_absent 'StoreScanStateRelaxed' "$ROOT_DIR/src/Heap/MAllocator/MSRegionInfo.h"
require_absent 'GetActiveHelpers' "$ROOT_DIR/src/Heap/MCollector/MSMarkWorkerPolicy.h"
require_absent 'IsRootStagingEmpty' "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
require_absent 'PeekHead' "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
require_absent 'PrependChain' "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
require_absent 'StealChainTo' "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
require_absent 'Swap' "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
require_absent 'UnitSpan' "$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.h"
require_absent 'RegionUsageSnapshot' \
    "$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.h" \
    "$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.cpp" \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h"
require_absent 'GetBorrowedRegionCount' \
    "$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.h" \
    "$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.cpp" \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h"
require_absent 'GetOwnedTotalRegionCount' \
    "$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.h" \
    "$ROOT_DIR/src/Heap/MAllocator/MSFreeManager.cpp" \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h"
require_absent 'GetActiveMSRegionCountReference' \
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h" \
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
require_absent 'GetSweptMSRegionCountReference' \
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h" \
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp" \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.h" \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
require_absent 'GetSweptHeapBytesReference' \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.h" \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
require_absent 'size_t GetHotFreeBytes\(\) const;' \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.h"
require_absent 'size_t GetHotFreeBytesReference\(\) const;' \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.h"
require_absent 'RegionSpaceMS::GetHotFreeBytes' \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
require_absent 'RegionSpaceMS::GetHotFreeBytesReference' \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
require_absent 'GetOwnedSegmentCount' "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h"
require_absent 'GetOwnedFreeRegionCount' "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h"
require_absent 'GetScavengedRegionCount' "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h"
require_absent 'GetHotFreeRegionCount' "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h"
scavenge_policy="$ROOT_DIR/src/Heap/MCollector/MSScavengeWatermarkPolicy.h"
watermark_decision=$(awk '/struct MSScavengeWatermarkDecision \{/,/^};/' "$scavenge_policy")
if printf '%s\n' "$watermark_decision" | \
    rg -q 'retainBudgetBytes|releaseBandBytes|budgetConstrained'; then
    echo 'MSScavengeWatermarkDecision exposes unused policy intermediates' >&2
    exit 1
fi
high_watermark_decision=$(awk '/struct MSScavengeHighWatermarkDecision \{/,/^};/' "$scavenge_policy")
if printf '%s\n' "$high_watermark_decision" | \
    rg -q 'desiredHighBytes|decayBytes|highRaised|highHeld|highDecayed'; then
    echo 'MSScavengeHighWatermarkDecision exposes unused policy intermediates' >&2
    exit 1
fi
require_absent 'IsMSSweepQuiescent' "$ROOT_DIR/src/Heap/MAllocator/MSParallelSweepPolicy.h"
require_absent 'MSMarkAssistActivation' "$ROOT_DIR/src/Heap/MCollector/MSAssistPacing.h"
require_absent 'FilteredWorkWords' "$ROOT_DIR/src/Heap/MCollector/MSAssistPacing.h"
require_absent 'gcCycle_' \
    "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h" \
    "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
require_absent 'targetSweepHelperCount_' \
    "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.h" \
    "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"
require_absent 'leaseMiss' "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"
require_absent 'objectCount' "$ROOT_DIR/src/Heap/MAllocator/MSMarkedStats.h"
require_absent 'Size' "$ROOT_DIR/src/Heap/MCollector/MSRootSnapshot.h"
require_absent 'MSMarkWorkerPolicy' "$ROOT_DIR/src/Heap/MCollector/MSMarkWorkerPolicy.h"
require_absent 'Empty' "$ROOT_DIR/src/Heap/MCollector/MSRootSnapshot.h"
require_absent 'Capacity' "$ROOT_DIR/src/Heap/MCollector/MSRootDedupTable.h"
require_absent 'ReservedCapacity' "$ROOT_DIR/src/Heap/MCollector/MSRootDedupTable.h"
require_absent 'UsedSlotCount' "$ROOT_DIR/src/Heap/MCollector/MSRootDedupTable.h"
require_absent 'HasMarkEpoch' \
    "$ROOT_DIR/src/Heap/MCollector/MSMarkBatch.h" \
    "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
require_absent 'HasBitmapPlaneBases' \
    "$ROOT_DIR/src/Heap/MCollector/MSMarkBatch.h" \
    "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
require_absent 'MSTerminationDrainLaneStats' "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h" "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
require_absent 'MSTerminationDrainStats' "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h" "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
require_absent 'ComputeMSMarkAssistRate' "$ROOT_DIR/src/Heap/MCollector/MSAssistPacing.h" "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
require_absent 'GetOwner' "$ROOT_DIR/src/Heap/MCollector/MSGCWorkerPoolController.h" "$ROOT_DIR/src/Heap/Collector/CollectorResources.cpp"
require_absent 'markWorkerCount_' "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h" "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
require_absent 'VisitRawPointers' "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.h"
require_absent 'ResolveMSScavengeTarget' "$ROOT_DIR/src/Heap/MCollector/MSMemoryLimitPolicy.h"
require_absent 'Reset' "$ROOT_DIR/src/Heap/MCollector/MSWorkUnit.h"
require_absent 'Size' "$ROOT_DIR/src/Heap/MCollector/MSMarkBatch.h"
require_absent 'MSMarkWorkerCaps' \
    "$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.h" \
    "$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.cpp" \
    "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
require_absent 'Phase phase' "$ROOT_DIR/src/Heap/MCollector/MSMarkWorkerControl.h"
require_absent 'const MSMarkReadyStack' "$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.h"
require_absent 'const MSMarkWorkHintShard' "$ROOT_DIR/src/Heap/MCollector/MSMarkScheduler.h"
require_absent 'FinishSweepSlice' \
    "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.h" \
    "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"
require_absent 'size_t Load() const' "$ROOT_DIR/src/Heap/MCollector/MSAssistPacing.h"
if rg -q 'GCThreadPool\* GetThreadPool\(\) const' \
    "$ROOT_DIR/src/Heap/MCollector/MSGCWorkerPoolSession.h"; then
    echo 'MSGC worker-pool session leaks its owned thread pool' >&2
    exit 1
fi

echo 'MSGC liveness cleanup contracts: PASS'
