#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

if rg -n '\bConcurrentSweep\b|concurrentSweepUs' "$HEADER" "$SOURCE"; then
    echo 'ConcurrentSweep must not remain as a collector phase or timing bucket' >&2
    exit 1
fi

python3 - "$SOURCE" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("void MarkSweepCollector::PostMarkTermination()")
end = source.index("void MarkSweepCollector::RunOOMEmergencyParallelSweep", start)
post_mark = source[start:end]

required = [
    "centralPool->ComputeMarkedStats(",
    "CollectLargeGarbage();",
    "UpdateGCStats();",
    "centralPool->PublishCollectSetForSweep();",
    "backgroundProcessor.NotifyToSweep(kNoPreferredSweepClass);",
]
cursor = 0
for token in required:
    found = post_mark.find(token, cursor)
    if found < 0:
        raise SystemExit(f"PostMarkTermination missing ordered sweep-publication step: {token}")
    cursor = found + len(token)

if "ProcessFinalizers" in post_mark or "NotifyFinalizerWork" in post_mark:
    raise SystemExit("PostMarkTermination still performs redundant finalizer notification")

do_gc_start = source.index("void MarkSweepCollector::DoGarbageCollection")
do_gc_end = source.index("// ============================================================================", do_gc_start)
if "ConcurrentSweep" in source[do_gc_start:do_gc_end]:
    raise SystemExit("DoGarbageCollection still invokes ConcurrentSweep")

run_start = source.index("void MarkSweepCollector::RunGarbageCollection")
run_end = source.index("void MarkSweepCollector::DoGarbageCollection", run_start)
if "UpdateGCStats();" in source[run_start:run_end]:
    raise SystemExit("RunGarbageCollection still updates the threshold after Sweep publication")
PY

echo 'MSGC PostMark sweep-publication contract holds'
