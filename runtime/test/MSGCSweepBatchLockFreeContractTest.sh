#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"

python3 - "$HEADER" "$SOURCE" <<'PY'
import pathlib
import sys

header = pathlib.Path(sys.argv[1]).read_text()
source = pathlib.Path(sys.argv[2]).read_text()

def block(text, begin, end):
    start = text.index(begin)
    finish = text.index(end, start)
    return text[start:finish]

assert "LockFreeRegionStack partialSwept_" in header
assert "LockFreeRegionStack fullSwept_" in header
assert "LockFreeRegionStack unswept_" in header
assert "partialUnswept_" not in header and "fullUnswept_" not in header, (
    "Step62 must use one unswept head per size/scan lane"
)
claim = block(header, "MSRegionInfo* ClaimUnswept()", "void PushPartialSwept")
assert "unswept_.Pop()" in claim and "lock_guard" not in claim, (
    "unified unswept claim must be one lock-free Pop"
)

assist = block(source, "size_t CentralRegionPool::AssistSweep",
               "bool CentralRegionPool::SweepOneUnsweptRegionForAssist")
assert "SweepOneUnsweptRegionForAssist" in assist
assert "targetRegions" in assist, "Assist slice must follow the requested target"

background = block(source, "void CentralRegionPool::BackgroundSweep",
                   "void CentralRegionPool::ParallelSweep")
parallel = block(source, "void CentralRegionPool::ParallelSweep",
                 "size_t CentralRegionPool::AssistSweep")
force = block(source, "void CentralRegionPool::ForceFinishUnsweptList",
              "void CentralRegionPool::ForceFinishAllUnswept")
common = block(source, "bool CentralRegionPool::SweepUnsweptList",
               "// ===== Monitoring =====")
assert "bucket.ClaimUnswept()" in common
assert "PushPartialSwept" in common and "PushFullSwept" in common
assert "SweepClassOrder" in background, "Background sweep must use the shared class-order loop"
assert "SweepClassOrder" in parallel, "Parallel helper must use the shared class-order loop"
assert "bucket.ClaimUnswept()" in force
assert "PushPartialSwept" in force and "PushFullSwept" in force

assert "PushChain" in header, "collect-set handoff must publish one intrusive chain"
assert "PublishCollectSetToUnswept" in header

print("MSGCSweepBatchLockFreeContractTest: PASS (unified Step62 head)")
PY
