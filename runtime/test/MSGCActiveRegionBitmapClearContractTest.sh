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
clear = section(
    source,
    "void CentralRegionPool::ClearAllMarkBitmaps()",
    "void CentralRegionPool::ReenableLazySweep()",
)

for forbidden in (
    "GetMaxRegionCount()",
    "maxRegions",
    "VisitAllRegions",
    "VisitLargeRegionsLocked",
):
    if forbidden in clear:
        raise SystemExit(
            "ClearAllMarkBitmaps must not scan committed metadata: "
            f"{forbidden}"
        )

for required in (
    "collectSet_[cls][scan].ForEach",
    "VisitAllocBufferMS",
    "VisitLargeCollectSetLocked",
    "ResetSmallMarkWorkState",
    "ClearMarkedFlag",
    "collectSetSmallCleared",
    "tlSmallCleared",
    "largeBaseCleared",
):
    if required not in clear:
        raise SystemExit(
            "ClearAllMarkBitmaps is missing active traversal step: "
            f"{required}"
        )

if "unordered_set" in clear:
    raise SystemExit(
        "ClearAllMarkBitmaps must not allocate a deduplication set in STW"
    )

print("MSGC active Region bitmap clear contract holds")
PY
