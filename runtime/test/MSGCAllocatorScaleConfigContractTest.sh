#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

python3 - \
    "$ROOT_DIR/src/CangjieRuntime.h" \
    "$ROOT_DIR/src/CangjieRuntime.cpp" \
    "$ROOT_DIR/src/CjScheduler.cpp" \
    "$ROOT_DIR/src/CangjieRuntimeApi.cpp" \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp" \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp" <<'PY'
import pathlib
import sys

runtime_header, runtime_source, scheduler_source, api_source, region_manager_source, region_space_source = (
    pathlib.Path(path).read_text() for path in sys.argv[1:]
)

for token in (
    "ConfigureMSAllocatorScale",
    "GetMSAllocatorScaleConfig",
):
    if token not in runtime_header:
        raise SystemExit(f"CangjieRuntime does not expose the internal MSGC scale config: {token}")

for token in (
    '"cjMSShardCount"',
    '"cjMSInitialSegmentCount"',
    "ResolveMSShardCount",
    "ResolveMSInitialSegmentCount",
):
    if token not in runtime_source:
        raise SystemExit(f"CangjieRuntime does not centrally resolve MSGC scale config: {token}")

if "ConfigureMSAllocatorScale(processorNum" not in scheduler_source:
    raise SystemExit("ordinary runtime entry does not use the shared MSGC scale resolver")

if "ConfigureMSAllocatorScale(resolvedProcessorNum" not in api_source:
    raise SystemExit("embedding runtime entry does not use the shared MSGC scale resolver")

for source_name, source in (
    ("CjScheduler.cpp", scheduler_source),
    ("CangjieRuntimeApi.cpp", api_source),
):
    if "initialSegmentCount" not in source:
        raise SystemExit(f"{source_name} still derives initial committed heap from processor/shard count")

for token in (
    "GetMSAllocatorScaleConfig()",
    "freeManager_.GetShardCount()",
    "TryComputeVirtualReserveLayout",
):
    if token not in region_manager_source:
        raise SystemExit(f"MSRegionManager does not consume the resolved allocator scale: {token}")

if "TryComputeVirtualReserveLayout" not in region_space_source:
    raise SystemExit("MSRegionSpace does not validate the complete virtual reserve layout")
PY

echo "MSGC allocator scale config contract holds"
