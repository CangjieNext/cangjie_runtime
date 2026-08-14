#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
MUTATOR_MANAGER="$ROOT_DIR/src/Mutator/MutatorManager.cpp"

python3 - "$COLLECTOR" "$HEADER" "$MUTATOR_MANAGER" <<'PY'
import pathlib
import sys

collector = pathlib.Path(sys.argv[1]).read_text()
header = pathlib.Path(sys.argv[2]).read_text()
mutator_manager = pathlib.Path(sys.argv[3]).read_text()

enum_start = collector.index("void MarkSweepCollector::EnumRoots()")
enum_end = collector.index("bool MarkSweepCollector::MarkObject", enum_start)
enum_roots = collector[enum_start:enum_end]

for token in ("TransitionAllMutatorsToGCPhase", "EnumAndMarkRoots", "PublishRootStaging"):
    assert token in enum_roots, f"EnumRoots is missing functional step: {token}"
assert "uint64_t startUs = TimeUtil::MicroSeconds();" in enum_roots, (
    "EnumRoots must start its own duration measurement"
)
assert 'VLOG(REPORT, "[MS-GC] EnumRoots: total=%sus"' not in enum_roots, (
    "EnumRoots must not emit a duplicate phase log"
)

for field in (
    "preEnumRootsUs",
    "enumRootsUs",
    "concurrentMarkUs",
    "markTerminationStwUs",
    "postMarkTerminationUs",
):
    assert field in header, f"MSGCycleStats is missing phase duration: {field}"

for function, field in (
    ("PreEnumRoots", "preEnumRootsUs"),
    ("EnumRoots", "enumRootsUs"),
    ("ConcurrentMark", "concurrentMarkUs"),
    ("MarkTermination", "markTerminationStwUs"),
    ("PostMarkTermination", "postMarkTerminationUs"),
):
    start = collector.index(f"void MarkSweepCollector::{function}")
    end = collector.find("\nvoid MarkSweepCollector::", start + 1)
    phase = collector[start:] if end == -1 else collector[start:end]
    assert field in phase, f"{function} does not record {field}"

concurrent_start = collector.index("void MarkSweepCollector::ConcurrentMark()")
concurrent_end = collector.index(
    "void MarkSweepCollector::RunSchedulerMarkLoop(", concurrent_start)
concurrent = collector[concurrent_start:concurrent_end]
assert 'VLOG(REPORT, "[MS-GC] === ConcurrentMark begin ===")' in concurrent, (
    "ConcurrentMark must report phase entry"
)
assert '"[MS-GC]   ConcurrentMark stats: totalWU=%llu' in concurrent, (
    "ConcurrentMark must report its WU breakdown before phase exit"
)
assert 'VLOG(REPORT, "[MS-GC] === ConcurrentMark end: total=%lluus ==="' in concurrent, (
    "ConcurrentMark phase exit must print only the recorded microseconds"
)

loop_start = collector.index("void MarkSweepCollector::RunSchedulerMarkLoop(")
loop_end = collector.index("size_t MarkSweepCollector::AcquireMarkWorkerPool", loop_start)
loop = collector[loop_start:loop_end]
assert "--- ConcurrentMark" not in loop, (
    "ConcurrentMark phase lifecycle logging belongs to ConcurrentMark, not its scheduler loop"
)

summary_start = collector.index("void MarkSweepCollector::LogGCSummary")
summary_end = collector.index("// ============================================================================\n// MS-GC specific UpdateGCStats", summary_start)
summary = collector[summary_start:summary_end]
assert "[MS-GC] GC total=" in summary, "GC summary must emit one phase-duration log"
for label in ("PreEnum=", "EnumRoots=", "ConcurrentMark=", "MarkTerminationSTW=", "PostMarkTermination="):
    assert label in summary, f"GC summary is missing phase label: {label}"

transition_start = mutator_manager.index("void MutatorManager::TransitionAllMutatorsToMarkSweepGCPhase")
transition_end = mutator_manager.index("bool MutatorManager::HasPendingEnumAssist", transition_start)
transition = mutator_manager[transition_start:transition_end]
for token in (
    "enumHandshakeInstallUs",
    "enumHandshakeBroadcastUs",
    "enumHandshakeWaitUs",
    "enumHandshakeAssistUs",
    "[MS-GC] EnumRoots handshake:",
):
    assert token not in transition, f"MarkSetup retains diagnostic timing: {token}"

print("MSGC cycle phase timing contract holds")
PY
