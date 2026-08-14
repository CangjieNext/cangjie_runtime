#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/Collector/Collector.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/Collector/Collector.cpp"
HEAP_SOURCE="$ROOT_DIR/src/Heap/Heap.cpp"
MARK_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
MARK_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

python3 - "$COLLECTOR_HEADER" "$COLLECTOR_SOURCE" "$HEAP_SOURCE" "$MARK_HEADER" "$MARK_SOURCE" <<'PY'
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


collector_header, collector_source, heap_source, mark_header, mark_source = [
    Path(path).read_text(encoding="utf-8") for path in sys.argv[1:]
]
mark_termination = extract_function(mark_source, "void MarkSweepCollector::MarkTermination(bool worldStopped)")
post_gc = extract_function(mark_source, "void MarkSweepCollector::PostGarbageCollection(uint64_t gcIndex)")

if "TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, false);" not in mark_termination:
    raise SystemExit("MarkTermination must publish IDLE before the STW scope ends")

if "TransitionToGCPhase(" in post_gc:
    raise SystemExit("PostGarbageCollection must not repeat the phase transition after MarkTermination")

for label, source in {
    "Collector.h": collector_header,
    "Collector.cpp": collector_source,
    "Heap.cpp": heap_source,
    "MarkSweepCollector.h": mark_header,
    "MarkSweepCollector.cpp": mark_source,
}.items():
    if "GC_PHASE_MS_SWEEP" in source:
        raise SystemExit(f"{label} must not retain GC_PHASE_MS_SWEEP")
PY

echo 'MSGC IDLE-after-MarkTermination contract holds'
