#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
WORKER_POLICY="$ROOT_DIR/src/Heap/MCollector/MSMarkWorkerPolicy.h"

python3 - "$HEADER" "$SOURCE" "$COLLECTOR" "$WORKER_POLICY" <<'PY'
import pathlib
import sys

header = pathlib.Path(sys.argv[1]).read_text()
source = pathlib.Path(sys.argv[2]).read_text()
collector = pathlib.Path(sys.argv[3]).read_text()
worker_policy = pathlib.Path(sys.argv[4]).read_text()

api = "PrepareCollectSetForMarkUnderSTW"
assert api in header, "CentralRegionPool must expose one PreEnum collect-set preparation entry"
assert f"uint32_t CentralRegionPool::{api}()" in source, (
    "CentralRegionPool must implement the combined PreEnum preparation entry"
)

start = source.index(f"uint32_t CentralRegionPool::{api}()")
end = source.index("size_t CentralRegionPool::GetCollectSetRegionCount() const", start)
prepare = source[start:end]

for token in (
    "StealSweptToCollectSet",
    "ResetSmallMarkWorkState",
    "region->collectEpoch = epoch",
    "VisitAllocBufferMS",
    "largeCollectSet_.swap(largeNextSupply_)",
    "ClearMarkedFlag",
    "PublishMSGCFastPathMarkEpoch(epoch)",
):
    assert token in prepare, f"combined PreEnum preparation is missing: {token}"

assert prepare.count("chain.ForEach") == 1, (
    "sealed Small collect-set Regions must be reset in the BuildCollectSet traversal"
)
assert prepare.index("ResetSmallMarkWorkState") < prepare.index("PublishMSGCFastPathMarkEpoch(epoch)"), (
    "all Small mark work state must be reset before publishing the new fast-path epoch"
)
assert prepare.index("VisitAllocBufferMS") < prepare.index("PublishMSGCFastPathMarkEpoch(epoch)"), (
    "TLS active mark work state must be reset before publishing the new epoch"
)
assert prepare.index("ClearMarkedFlag") < prepare.index("PublishMSGCFastPathMarkEpoch(epoch)"), (
    "LARGE mark state must be reset before publishing the new epoch"
)

pre_start = collector.index("void MarkSweepCollector::PreEnumRoots()")
pre_end = collector.index("void MarkSweepCollector::EnumRoots()", pre_start)
pre_enum = collector[pre_start:pre_end]
assert f"centralPool->{api}()" in pre_enum, "PreEnumRoots must use combined preparation"
assert "centralPool->BuildCollectSet();" not in pre_enum, (
    "PreEnumRoots must not retain the separate BuildCollectSet pass"
)
assert "centralPool->ClearAllMarkBitmaps();" not in pre_enum, (
    "PreEnumRoots must not retain the separate collect-set bitmap-clear pass"
)

for token in (
    "ComputeMSPreEnumResetHelperTarget",
    "resources.GetMSGCThreadPoolHelperLimit()",
    "estimatedResetBytes",
    "TryAcquireMSGCWorkerPoolForPreEnum",
    "threadPool->WaitFinish()",
    "parallelReset",
):
    assert token in prepare, f"large collect-set parallel reset is missing: {token}"

assert "kMSPreEnumResetFirstHelperBytes = 1 * 1024 * 1024" in worker_policy, (
    "parallel reset must start its first helper at 1MiB"
)

assert prepare.index("threadPool->WaitFinish()") < prepare.index("PublishMSGCFastPathMarkEpoch(epoch)"), (
    "parallel reset tasks must finish before publishing the new fast-path epoch"
)

print("MSGC PreEnum combined collect-set preparation contract holds")
PY
