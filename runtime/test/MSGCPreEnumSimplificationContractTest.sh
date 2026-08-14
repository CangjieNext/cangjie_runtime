#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR_H="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
COLLECTOR_CPP="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
CENTRAL_H="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
CENTRAL_CPP="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"

python3 - "$COLLECTOR_H" "$COLLECTOR_CPP" "$CENTRAL_H" "$CENTRAL_CPP" <<'PY'
import pathlib
import sys

collector_h, collector_cpp, central_h, central_cpp = [pathlib.Path(p).read_text() for p in sys.argv[1:]]

for obsolete in ("markBatchEpoch_", "markBatchAllocPlaneBase_", "markBatchMarkPlaneBase_"):
    assert obsolete not in collector_h and obsolete not in collector_cpp, (
        f"collector must not cache {obsolete}; batches read the published fast-path context")

context_load = collector_cpp[collector_cpp.index("MSMarkBitmapContext MarkSweepCollector::LoadCurrentMSMarkBitmapContext"):]
context_load = context_load[:context_load.index("void MarkSweepCollector::InitializeGCMarkBatch")]
for required in ("CJ_MRT_MSGCFastPathState.currentMarkEpoch",
                 "CJ_MRT_MSGCFastPathState.allocPlaneBase",
                 "CJ_MRT_MSGCFastPathState.markPlaneBase",
                 "__ATOMIC_ACQUIRE"):
    assert required in context_load, f"GC mark bitmap context must acquire published {required}"

batch_init = collector_cpp[collector_cpp.index("void MarkSweepCollector::InitializeGCMarkBatch"):]
batch_init = batch_init[:batch_init.index("// Runtime slow-path view")]
assert "LoadCurrentMSMarkBitmapContext()" in batch_init, (
    "GC mark batches must initialize through the shared published bitmap context")

assert "PrepareMarkEpochUnderSTW" in central_h
prepare_start = central_cpp.index("uint32_t CentralRegionPool::PrepareMarkEpochUnderSTW")
prepare_end = central_cpp.index("void CentralRegionPool::PublishCollectSetForSweep()", prepare_start)
prepare = central_cpp[prepare_start:prepare_end]
assert "ResetMSBarrierMarkBuffer" in prepare and "ResetGCMarkAssistCycleState" in prepare, (
    "central epoch preparation must discard stale barrier and assist state")
assert prepare.index("ResetMSBarrierMarkBuffer") < prepare.index("PublishMSGCFastPathMarkEpoch(epoch)"), (
    "barrier state must reset before the new epoch is published")

pre_start = collector_cpp.index("void MarkSweepCollector::PreEnumRoots")
pre_end = collector_cpp.index("void MarkSweepCollector::EnumRoots", pre_start)
pre = collector_cpp[pre_start:pre_end]
assert "PrepareBarrierMarkBuffersForNewEpochUnderSTW" not in pre
assert "PrepareMarkEpochUnderSTW(gcReason == GC_REASON_OOM)" in pre

prepare_start = central_cpp.index('uint32_t CentralRegionPool::PrepareMarkEpochUnderSTW(')
prepare_end = central_cpp.index('void CentralRegionPool::PublishCollectSetForSweep()', prepare_start)
prepare = central_cpp[prepare_start:prepare_end]
lane_start = prepare.index('struct ResetLane')
lane_end = prepare.index('};', lane_start) + len('};')
lane = prepare[lane_start:lane_end]
assert 'RegionChain* chain;' in lane, (
    'PRE_ENUM reset lanes must retain only the chain consumed by reset work'
)
assert 'uint8_t cls;' not in lane and 'uint8_t scan;' not in lane, (
    'PRE_ENUM reset lanes must not carry unused class or scan metadata'
)
assert "PreEnumRoots: stage-times" not in pre

for obsolete in ("markSetupUs", "postMarkUs", "postGcHookUs",
                 "markPhaseTransitionUs", "markPacingSetupUs", "markPoolAcquireUs",
                 "markWorkerStartUs", "markLoopUs", "markQuiesceUs", "markPoolReleaseUs",
                 "markSchedulerUs"):
    assert obsolete not in collector_h and obsolete not in collector_cpp, (
        f"per-stage diagnostic timing {obsolete} must be removed")

print("MSGC PreEnum simplification contract holds")
PY
