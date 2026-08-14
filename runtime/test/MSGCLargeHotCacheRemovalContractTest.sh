#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT_DIR" <<'PY'
from pathlib import Path
import sys


root = Path(sys.argv[1])
paths = [
    root / "src/Heap/MAllocator/MSCentralRegionPool.h",
    root / "src/Heap/MAllocator/MSCentralRegionPool.cpp",
    root / "src/Heap/MAllocator/MSRegionSpace.h",
    root / "src/Heap/MAllocator/MSRegionSpace.cpp",
    root / "src/Heap/MCollector/BackgroundProcessor.cpp",
    root / "src/Heap/MCollector/MarkSweepCollector.cpp",
]
sources = {path: path.read_text(encoding="utf-8") for path in paths}
combined = "\n".join(sources.values())

for forbidden in (
    "largeHotCache",
    "LargeHotCache",
    "largeCache",
    "LargeHotEntry",
):
    if forbidden in combined:
        raise SystemExit(f"large-object hot-cache symbol remains: {forbidden}")

pool_source = sources[root / "src/Heap/MAllocator/MSCentralRegionPool.cpp"]
sweep_start = pool_source.index("void CentralRegionPool::SweepLargeRegions()")
sweep_end = pool_source.index("// ============================================================================\n// Per-thread AllocBufferMS registry", sweep_start)
sweep = pool_source[sweep_start:sweep_end]
for token in ("uint64_t startUs = TimeUtil::MicroSeconds();",
              "releasedLargeRegions", "reclaimedBytes",
              "SweepLargeRegions: releasedLargeRegions=%zu reclaimedBytes=%zu total=%sus"):
    if token not in sweep:
        raise SystemExit(f"SweepLargeRegions missing release timing accounting: {token}")
sweep_end = pool_source.index(
    "// ============================================================================\n"
    "// Per-thread AllocBufferMS registry",
    sweep_start,
)
sweep = pool_source[sweep_start:sweep_end]
if "msRegionManager_->ReclaimRegionLarge(regionStart, regionBytes);" not in sweep:
    raise SystemExit("dead large spans must return directly to the unified FreeManager")

space_source = sources[root / "src/Heap/MAllocator/MSRegionSpace.cpp"]
alloc_start = space_source.index("MAddress RegionSpaceMS::TryAllocateLargeObjectOnce(")
alloc_end = space_source.index("MAddress RegionSpaceMS::TryAllocateRegularObjectOnce(", alloc_start)
large_alloc = space_source[alloc_start:alloc_end]
if "TakeRegionForLarge" not in large_alloc:
    raise SystemExit("large allocation must use the unified FreeManager path")

print("MSGCLargeHotCacheRemovalContractTest: PASS")
PY
