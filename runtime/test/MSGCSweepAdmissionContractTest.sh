#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
COORDINATOR_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSSweepEpochCoordinator.h"
COORDINATOR_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSSweepEpochCoordinator.cpp"

python3 - "$HEADER" "$SOURCE" "$COORDINATOR_HEADER" "$COORDINATOR_SOURCE" <<'PY'
import pathlib
import re
import sys

header = pathlib.Path(sys.argv[1]).read_text()
source = pathlib.Path(sys.argv[2]).read_text()
coordinator_header = pathlib.Path(sys.argv[3]).read_text()
coordinator_source = pathlib.Path(sys.argv[4]).read_text()

def block(text, begin, end):
    start = text.index(begin)
    finish = text.index(end, start)
    return text[start:finish]

assert "MSSweepEpochCoordinator sweepEpoch_" in header, (
    "CentralRegionPool must delegate epoch state to MSSweepEpochCoordinator"
)
assert "std::atomic<uint64_t> admission_" in coordinator_header, (
    "coordinator must use one atomic admission word"
)
assert "std::mutex quiescenceMtx_" in coordinator_header, (
    "coordinator needs a dedicated quiescence mutex"
)
assert "std::condition_variable quiescenceCV_" in coordinator_header, (
    "coordinator needs a dedicated quiescence condition variable"
)
for symbol in ("TryEnterSweep", "LeaveSweep", "CloseSweepAdmissionAndWait", "OpenSweepAdmission"):
    assert symbol in coordinator_header and symbol in coordinator_source, f"missing coordinator API: {symbol}"

leave = block(coordinator_source, "void MSSweepEpochCoordinator::LeaveSweep()", "void MSSweepEpochCoordinator::CloseSweepAdmissionAndWait()")
assert "kAdmissionClosed" in leave and "kAdmissionActiveMask" in leave
assert "quiescenceMtx_" in leave and "quiescenceCV_.notify_one()" in leave, (
    "the last admitted Sweep worker must notify closed-admission quiescence"
)

close = block(coordinator_source, "void MSSweepEpochCoordinator::CloseSweepAdmissionAndWait()", "void MSSweepEpochCoordinator::OpenSweepAdmission()")
assert "SWEEP_QUIESCENCE_SPIN_US = 20" in coordinator_source
assert "SweepQuiescenceSpinPause()" in close
assert "quiescenceMtx_" in close and "quiescenceCV_.wait(" in close
assert "std::this_thread::yield()" not in close and "sched_yield()" not in close, (
    "Sweep quiescence must not yield the GC main thread to the OS scheduler"
)

assert "PublishSweepDoneAfterQuiescence" in coordinator_header
assert "PublishSweepDoneAfterQuiescence" in source
assert "sweepEpoch_.PublishSweepDoneAfterQuiescence()" in source

for begin, end in (
    ("void CentralRegionPool::BackgroundSweep", "void CentralRegionPool::ParallelSweep"),
    ("void CentralRegionPool::ParallelSweep", "size_t CentralRegionPool::AssistSweep"),
    ("size_t CentralRegionPool::AssistSweep", "bool CentralRegionPool::SweepOneUnsweptRegionForAssist"),
):
    assert "PublishSweepDoneAfterQuiescence" not in block(source, begin, end), (
        f"worker path must not publish sweep done: {begin}"
    )

for obsolete in (
    "void ParallelSweepOnceForOOM(uint8_t preferredCls);",
    "void ForceFinishAllUnsweptForOOM();",
    "void CentralRegionPool::ParallelSweepOnceForOOM(uint8_t preferredCls)",
    "void CentralRegionPool::ForceFinishAllUnsweptForOOM()",
):
    assert obsolete not in header and obsolete not in source, (
        f"unused OOM-only Sweep path must be removed: {obsolete}"
    )

stop = block(source, "void CentralRegionPool::StopAndWaitForAllSweep()", "void CentralRegionPool::VisitMarkPendingRegionsUnderSTW(")
assert stop.index("sweepEpoch_.CloseSweepAdmissionAndWait") < stop.index("PublishSweepDoneAfterQuiescence")

reenable = block(source, "void CentralRegionPool::ReenableSweepAdmission()", "} // namespace MapleRuntime")
assert "sweepEpoch_.OpenSweepAdmission" in reenable

publish = block(source, "void CentralRegionPool::PublishCollectSetForSweep()",
                "void CentralRegionPool::BackgroundSweep")
assert "ReenableSweepAdmission()" in publish, (
    "new Sweep work must be published before ordinary Sweep admission opens"
)
assert publish.index("sweepEpoch_.BeginEpoch(totalUnswept != 0)") < \
    publish.index("ReenableSweepAdmission()")

print("MSGCSweepAdmissionContractTest: PASS")
PY
