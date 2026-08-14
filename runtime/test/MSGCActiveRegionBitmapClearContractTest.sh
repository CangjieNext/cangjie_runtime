#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CENTRAL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"

python3 - "$CENTRAL_SOURCE" <<'PY'
import pathlib
import sys


def section(source: str, start_token: str, end_token: str) -> str:
    start = source.index(start_token)
    end = source.index(end_token, start)
    return source[start:end]


source = pathlib.Path(sys.argv[1]).read_text()
prepare = section(
    source,
    "uint32_t CentralRegionPool::PrepareMarkEpochUnderSTW(bool forceMaxHelpers)",
    "void CentralRegionPool::PublishCollectSetForSweep()",
)

for forbidden in (
    "GetMaxRegionCount()",
    "maxRegions",
    "VisitAllRegions",
    "VisitLargeRegionsLocked",
):
    if forbidden in prepare:
        raise SystemExit(
            "PreEnum collect-set preparation must not scan committed metadata: "
            f"{forbidden}"
        )

for required in (
    "StealSweptToCollectSet",
    "chain.ForEach",
    "VisitAllocBufferMS",
    "largeCollectSet_.swap(largeNextSupply_)",
    "ResetSmallMarkWorkState",
    "ClearMarkedFlag",
):
    if required not in prepare:
        raise SystemExit(
            "PreEnum collect-set preparation is missing active traversal step: "
            f"{required}"
        )

if "unordered_set" in prepare:
    raise SystemExit(
        "PreEnum collect-set preparation must not allocate a deduplication set in STW"
    )

print("MSGC active Region bitmap clear contract holds")
PY
