#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ACCOUNTING_SOURCE="$ROOT_DIR/src/Heap/MCollector/MSHeapAccounting.cpp"

python3 - "$ACCOUNTING_SOURCE" <<'PY'
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
retention_capture = extract_function(
    source,
    "MSHeapRetentionSnapshot CaptureMSHeapRetentionSnapshotImpl",
)

if "GetActiveMSRegionCount()" not in retention_capture:
    raise SystemExit(
        "retention capture must reuse the incremental active-Region aggregate"
    )

for forbidden in (
    "GetRegionUsageSnapshot",
    "GetLargeHotCacheRegionCount",
):
    if forbidden in retention_capture:
        raise SystemExit(
            "retention capture must not compute a synchronous Region snapshot: "
            f"{forbidden}"
        )
PY

echo "MSGC heap-retention fast-path contract holds"
