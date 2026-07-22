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

assert "kSweepBatchSize = 64" in header, "shared sweep batch size must be 64"
assert "ClaimUnsweptBatch" in header and "ClaimUnsweptBatch" in source, (
    "CentralRegionPool must expose a batched unswept claim"
)
assert "PushPartialSweptBatch" in header
assert "PushFullSweptBatch" in header
assert "PushEmptySweptBatch" in header and "PushEmptySweptBatch" in source

assert "LockFreeRegionStack partialUnswept_" in header, (
    "partial unswept list must use the tagged lock-free stack"
)
assert "LockFreeRegionStack fullUnswept_" in header, (
    "full unswept list must use the tagged lock-free stack"
)
assert "mutable std::mutex lock_;              // protects fullSwept_" in header, (
    "bucket mutex must not protect unswept lists"
)
claim = block(header, "size_t ClaimUnsweptBatch", "void PushPartialSwept")
assert "PopBatch" in claim and "lock_guard" not in claim, (
    "unswept batch claim must be lock-free"
)

lazy = block(source, "MSRegionInfo* CentralRegionPool::TakeUnsweptRegion",
             "MSRegionInfo* CentralRegionPool::GetFreshRegion")
assert "ClaimUnsweptHeadForSweep" in lazy, "Lazy sweep must retain single-Region claim"
assert "ClaimUnsweptBatch" not in lazy, "Lazy sweep must not claim a batch"

assist = block(source, "size_t CentralRegionPool::AssistSweep",
               "bool CentralRegionPool::SweepUnsweptList")
assert "ClaimUnsweptBatch" in assist, "Assist sweep must claim in one batch"
assert "targetRegions" in assist, "Assist batch size must follow the requested target"

background = block(source, "void CentralRegionPool::BackgroundSweep",
                   "void CentralRegionPool::ParallelSweep")
parallel = block(source, "void CentralRegionPool::ParallelSweep",
                 "MSSweepAssistResult CentralRegionPool::MSSweepAssist")
force = block(source, "void CentralRegionPool::ForceFinishUnsweptList",
              "void CentralRegionPool::ForceFinishAllUnswept")
common = block(source, "bool CentralRegionPool::SweepUnsweptList",
               "// ===== Monitoring =====")
assert "kSweepBatchSize" in common, "shared sweep loop must use the 64-Region batch"
assert "ClaimUnsweptBatch" in common and "PublishSweptBatch" in common, (
    "shared sweep loop must claim and publish classified batches"
)
assert "SweepUnsweptList" in background, "Background sweep must use the shared batch loop"
assert "SweepUnsweptList" in parallel, "Parallel helper must use the shared batch loop"
assert "kSweepBatchSize" in force, "GC force sweep must use the shared 64-Region batch"
assert "PublishSweptBatch" in force, "GC force sweep must publish classified results in bulk"

assert "PushBatch" in header, "lock-free stack must support one-CAS batch publication"
assert "PopBatch" in header, "lock-free stack must support one-CAS batch claim"

print("MSGCSweepBatchLockFreeContractTest: PASS")
PY
