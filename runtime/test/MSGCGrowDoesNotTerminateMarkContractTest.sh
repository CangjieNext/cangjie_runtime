#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REGION_MANAGER_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
MARK_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
MARK_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
TERMINATION_POLICY="$ROOT_DIR/src/Heap/MCollector/MSMarkTerminationPolicy.h"

python3 - \
    "$REGION_MANAGER_SOURCE" \
    "$MARK_HEADER" \
    "$MARK_SOURCE" \
    "$TERMINATION_POLICY" <<'PY'
from pathlib import Path
import sys


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise SystemExit(f"unterminated function: {signature}")


region_source, mark_header, mark_source, policy = [
    Path(path).read_text(encoding="utf-8") for path in sys.argv[1:]
]
take_regions = extract_function(region_source, "bool MSRegionManager::TakeRegions(")
concurrent_mark = extract_function(mark_source, "void MarkSweepCollector::RunConcurrentMarkLoop()")

if "RequestActiveMarkTerminationBeforeGrow" in take_regions:
    raise SystemExit("TakeRegions must grow without requesting MarkTermination")

for label, source in {
    "MarkSweepCollector.h": mark_header,
    "MarkSweepCollector.cpp": mark_source,
}.items():
    for forbidden in (
        "RequestActiveMarkTerminationBeforeGrow",
        "requestedGrowRegions_",
        "largeGrowRequested_",
        "concurrentMarkRunning_",
        "markGrowRequestRegions",
    ):
        if forbidden in source:
            raise SystemExit(f"{label} must not retain grow termination state: {forbidden}")

for forbidden in (
    "SMALL_NEEDS_GROW",
    "LARGE_NEEDS_GROW",
    "ResolveMSMarkGrowTerminationReason",
    "small-needs-grow",
    "large-needs-grow",
):
    if forbidden in policy:
        raise SystemExit(f"termination policy must not retain grow reason: {forbidden}")

if "requestedGrowRegions" in concurrent_mark:
    raise SystemExit("ConcurrentMark must not inspect grow requests")
PY

echo 'MSGC grow is independent from MarkTermination'
