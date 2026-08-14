#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
OOM_POLICY="$ROOT_DIR/src/Heap/MAllocator/MSOOMEmergencyPolicy.h"

if rg -n '\bConcurrentSweep\b|concurrentSweepUs' "$HEADER" "$SOURCE"; then
    echo 'ConcurrentSweep must not remain as a collector phase or timing bucket' >&2
    exit 1
fi

python3 - "$SOURCE" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("void MarkSweepCollector::PostMarkTermination()")
brace = source.index("{", start)
depth = 0
for end in range(brace, len(source)):
    if source[end] == "{":
        depth += 1
    elif source[end] == "}":
        depth -= 1
        if depth == 0:
            post_mark = source[start:end + 1]
            break
else:
    raise SystemExit("unterminated PostMarkTermination")

required = [
    '"[MS-GC] === PostMarkTermination begin ==="',
    "CHECK(centralPool != nullptr);",
    "centralPool->ComputeMarkedStats()",
    "centralPool->SweepLargeRegions();",
    "UpdateGCStats();",
    "centralPool->PublishCollectSetForSweep();",
    '"[MS-GC] === PostMarkTermination end: total=%lluus ==="',
    'LogRegionSummary("GC end");',
    "backgroundProcessor.NotifyToSweep(kNoPreferredSweepClass)",
    "backgroundProcessor.NotifyToScavenge()",
]
cursor = 0
for token in required:
    found = post_mark.find(token, cursor)
    if found < 0:
        raise SystemExit(f"PostMarkTermination missing ordered sweep-publication step: {token}")
    cursor = found + len(token)

if "if (centralPool != nullptr)" in post_mark or "else {" in post_mark:
    raise SystemExit("PostMarkTermination must treat CentralRegionPool as a required MSGC invariant")

if "ProcessFinalizers" in post_mark or "NotifyFinalizerWork" in post_mark:
    raise SystemExit("PostMarkTermination still performs redundant finalizer notification")

if "non-STW" in source[source.index("// Step 2.4: PostMarkTermination"):source.index("void MarkSweepCollector::PostMarkTermination()")]:
    raise SystemExit("PostMarkTermination comment must describe the OOM STW caller")

do_gc_start = source.index("void MarkSweepCollector::DoGarbageCollection")
do_gc_end = source.index("// ============================================================================", do_gc_start)
if "ConcurrentSweep" in source[do_gc_start:do_gc_end]:
    raise SystemExit("DoGarbageCollection still invokes ConcurrentSweep")

run_start = source.index("void MarkSweepCollector::RunGarbageCollection")
run_end = source.index("void MarkSweepCollector::DoGarbageCollection", run_start)
run_gc = source[run_start:run_end]
if "UpdateGCStats();" in run_gc:
    raise SystemExit("RunGarbageCollection must not update the threshold after Sweep publication")
if "NotifyToScavenge" in run_gc:
    raise SystemExit("RunGarbageCollection must leave Scavenge notification to PostMarkTermination")
PY

if rg -q 'MSPostMarkSweepPlan|BuildMSPostMarkSweepPlan|ResolveMSInitialMarkHelpers' \
    "$SOURCE" "$HEADER" "$OOM_POLICY"; then
    echo 'MSGC retains an inert post-mark OOM sweep plan' >&2
    exit 1
fi

echo 'MSGC PostMark sweep-publication contract holds'
