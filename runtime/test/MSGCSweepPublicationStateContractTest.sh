#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"

python3 - "$HEADER" "$SOURCE" <<'PY'
from pathlib import Path
import sys

header = Path(sys.argv[1]).read_text(encoding="utf-8")
source = Path(sys.argv[2]).read_text(encoding="utf-8")

publish_start = source.index("void CentralRegionPool::PublishCollectSetForSweep()")
publish_end = source.index("void CentralRegionPool::BackgroundSweep", publish_start)
publish = source[publish_start:publish_end]

required = [
    "PublishCollectSetToUnswept",
    "centralSmallUnsweptRegions_.store(totalUnswept",
    "sweepCycleState_.ResetForNewEpoch(sweepAssistRatio)",
    "sweepEpoch_.BeginEpoch(totalUnswept != 0)",
    "regionManager->AdvanceScavengeEpoch(newSweepCycleId)",
    "ReenableSweepAdmission()",
    '"[MS-GC] Publish sweep: epoch=%u unsweptRegions=%zu assistRatio=%llu"',
]
for token in required:
    if token not in publish:
        raise SystemExit(f"sweep publication missing: {token}")

if publish.index("sweepEpoch_.BeginEpoch(totalUnswept != 0)") > \
        publish.index("ReenableSweepAdmission()"):
    raise SystemExit("Sweep admission must open only after the new epoch is published")

for forbidden in ("startUs", "total=%sus", "mutatorAssistDisabled"):
    if forbidden in publish:
        raise SystemExit(f"sweep publication retains redundant timing/diagnostic state: {forbidden}")

bucket_start = header.index("void PublishCollectSetToUnswept(RegionChain& source)")
bucket_end = header.index("size_t CountPartialSwept() const", bucket_start)
bucket = header[bucket_start:bucket_end]
if "AddAggregate(unsweptRegions_" in bucket:
    raise SystemExit("bucket publication must not increment the aggregate before the single final store")

state_start = header.index("struct MSSweepCycleState")
state_end = header.index("std::atomic<bool> bgSweepRunning_", state_start)
state = header[state_start:state_end]
for token in ("assistRatioScaled", "MSSweepCreditPool", "pressureMode", "forceHighPressure", "ResetForNewEpoch"):
    if token not in state:
        raise SystemExit(f"sweep cycle state missing: {token}")
for forbidden in ("MSSweepEpochCoordinator", "centralSmallUnsweptRegions_", "stopSweepRequested_"):
    if forbidden in state:
        raise SystemExit(f"sweep cycle state must not own protocol or aggregate state: {forbidden}")
PY

echo 'MSGC sweep publication state contract holds'
