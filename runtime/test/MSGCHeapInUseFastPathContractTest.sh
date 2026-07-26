#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SPACE_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"

python3 - "$SPACE_SOURCE" <<'PY'
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


source = Path(sys.argv[1]).read_text(encoding="utf-8")
heap_in_use = extract_function(
    source,
    "size_t RegionSpaceMS::GetHeapInUseBytes() const",
)

if "return GetUsedPageSize();" not in heap_in_use:
    raise SystemExit(
        "GetHeapInUseBytes must reuse the incremental active-Region snapshot"
    )

for forbidden in (
    "GetRegionUsageSnapshot",
    "GetEmptySweptRegionCount",
    "GetLargeHotCacheRegionCount",
):
    if forbidden in heap_in_use:
        raise SystemExit(
            "GetHeapInUseBytes must not compute a synchronous Region snapshot: "
            f"{forbidden}"
        )
PY

echo "MSGC heap-in-use fast-path contract holds"
