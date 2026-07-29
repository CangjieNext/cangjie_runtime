#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"

python3 - "$HEADER" "$SOURCE" <<'PY'
import pathlib
import re
import sys

header = pathlib.Path(sys.argv[1]).read_text()
source = pathlib.Path(sys.argv[2]).read_text()

def block(text, begin, end):
    start = text.index(begin)
    finish = text.index(end, start)
    return text[start:finish]

assert "std::atomic<uint64_t> sweepAdmission_" in header, (
    "CentralRegionPool must use one atomic admission word"
)
for symbol in ("TryEnterSweep", "LeaveSweep", "CloseSweepAdmissionAndWait", "OpenSweepAdmission"):
    assert symbol in header and symbol in source, f"missing sweep admission API: {symbol}"

assert "PublishSweepDoneAfterQuiescence" in header
assert "PublishSweepDoneAfterQuiescence" in source
assert "TryPublishSweepDone" not in source

for begin, end in (
    ("void CentralRegionPool::BackgroundSweep", "void CentralRegionPool::ParallelSweep"),
    ("void CentralRegionPool::ParallelSweep", "void CentralRegionPool::ParallelSweepOnceForOOM"),
    ("size_t CentralRegionPool::AssistSweep", "bool CentralRegionPool::SweepOneUnsweptRegionForAssist"),
):
    assert "PublishSweepDoneAfterQuiescence" not in block(source, begin, end), (
        f"worker path must not publish sweep done: {begin}"
    )

oom = block(source, "void CentralRegionPool::ForceFinishAllUnsweptForOOM()", "size_t CentralRegionPool::CountTLActiveRegions")
assert "PublishSweepDoneAfterQuiescence" not in oom, "OOM path must not publish sweep done"

stop = block(source, "void CentralRegionPool::StopAndWaitForAllSweep()", "void CentralRegionPool::ClearAllMarkBitmaps()")
assert stop.index("CloseSweepAdmissionAndWait") < stop.index("PublishSweepDoneAfterQuiescence")

reenable = block(source, "void CentralRegionPool::ReenableLazySweep()", "} // namespace MapleRuntime")
assert "OpenSweepAdmission" in reenable

print("MSGCSweepAdmissionContractTest: PASS")
PY
