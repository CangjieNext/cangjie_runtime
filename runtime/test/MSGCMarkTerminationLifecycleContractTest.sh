#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MARK_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
MARK_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

python3 - "$MARK_HEADER" "$MARK_SOURCE" <<'PY'
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


header = Path(sys.argv[1]).read_text(encoding="utf-8")
source = Path(sys.argv[2]).read_text(encoding="utf-8")
termination = extract_function(
    source, "void MarkSweepCollector::MarkTermination(bool worldStopped)")
ordinary_publish = extract_function(
    source, "void MarkSweepCollector::PublishOrdinaryMarkTasksUnderSTW(")
finalizer_publish = extract_function(
    source, "void MarkSweepCollector::PublishFinalizerResurrectionRootsUnderSTW(")
drain_with_helpers = extract_function(
    source, "MarkSweepCollector::DrainSchedulerWithHelpersUnderSTW()")
verify_completeness = extract_function(
    source, "void MarkSweepCollector::VerifyMarkCompleteness()")
verify_quiescence = extract_function(
    source, "void MarkSweepCollector::VerifyMarkSchedulerQuiescence(")

if "[MS-GC] === MarkTermination begin ===" not in termination:
    raise SystemExit("MarkTermination must report phase entry")
if "[MS-GC]   MarkTermination stats: drainWU=%llu" not in termination:
    raise SystemExit("MarkTermination must report drain WU before phase exit")
if "[MS-GC] === MarkTermination end: total=%lluus ===" not in termination:
    raise SystemExit("MarkTermination must report a microsecond-only phase exit")
if "cycleStats_.markTerminationStwUs =" not in termination:
    raise SystemExit("MarkTermination must preserve STW phase accounting")
if "static_cast<unsigned long long>(cycleStats_.markTerminationStwUs)" not in termination:
    raise SystemExit("MarkTermination exit log must print recorded STW microseconds")

if "CHECK(centralPool != nullptr);" not in termination:
    raise SystemExit("MarkTermination requires a CentralRegionPool")
if "if (centralPool != nullptr)" in termination:
    raise SystemExit("MarkTermination must not retain optional CentralRegionPool branches")
for required in (
    "PublishOrdinaryMarkTasksUnderSTW(*centralPool);",
    "DrainSchedulerWithHelpersUnderSTW();",
    "PublishFinalizerResurrectionRootsUnderSTW(resurrectionPublishWork);",
    "ClearWeakRefsAfterMark();",
):
    if required not in termination:
        raise SystemExit(f"MarkTermination must use required central pool: {required}")

sequence = [
    termination.index("PublishOrdinaryMarkTasksUnderSTW(*centralPool);"),
    termination.index("DrainSchedulerWithHelpersUnderSTW();"),
    termination.index("PublishFinalizerResurrectionRootsUnderSTW(resurrectionPublishWork);"),
    termination.index("DrainSchedulerWithHelpersUnderSTW();",
                      termination.index("PublishFinalizerResurrectionRootsUnderSTW(resurrectionPublishWork);")),
    termination.index("ClearWeakRefsAfterMark();"),
]
if sequence != sorted(sequence):
    raise SystemExit("MarkTermination ordinary/finalizer/weak phase order is invalid")
if "ReenableSweepAdmission" in termination:
    raise SystemExit("MarkTermination must not open Sweep admission before Sweep publication")

verify_gate = termination.find("if (IsVerifyMarkEnabled())")
verify_steps = [
    termination.find("VerifyMarkCompleteness();"),
    termination.find('VerifyMarkSchedulerQuiescence("ordinary");'),
    termination.find('VerifyMarkSchedulerQuiescence("resurrection");'),
]
if verify_gate < 0 or any(step < verify_gate for step in verify_steps):
    raise SystemExit("MarkTermination must gate every post-mark verification centrally")
if verify_completeness.count("IsVerifyMarkEnabled()") != 0 or \
        verify_quiescence.count("IsVerifyMarkEnabled()") != 0:
    raise SystemExit("post-mark verification helpers must not repeat the outer gate")

if "DrainOrdinaryClosureToFixedPointUnderSTW" in source or \
        "MSTerminationClosureStats" in header:
    raise SystemExit("MarkTermination must not retain the combined ordinary closure helper")
if "FlushAllBarrierMarkBuffersUnderSTW(centralPool);" not in ordinary_publish:
    raise SystemExit("ordinary task publication must flush barrier mark buffers")
if "centralPool.VisitMarkPendingRegionsUnderSTW(" not in ordinary_publish:
    raise SystemExit("ordinary task publication must inspect all pending mark regions")
if "MSScanState::OWNED" not in ordinary_publish:
    raise SystemExit("ordinary task publication must reject an owned Region after quiescence")
if "markScheduler_.TryPublishRegion(" not in ordinary_publish:
    raise SystemExit("ordinary task publication must enqueue residual Region work")
if "PrepareFinalizersForResurrection();" not in finalizer_publish:
    raise SystemExit("finalizer publication must classify finalizers after ordinary drain")
if "DrainSchedulerSeriallyUnderSTW();" not in drain_with_helpers:
    raise SystemExit("helper drain must retain a serial fallback")
if "DrainSchedulerWithHelpersUnderSTW" not in header or \
        "DrainSchedulerSeriallyUnderSTW" not in header:
    raise SystemExit("shared scheduler drain helpers must be declared")
if "DrainOrdinarySchedulerUnderSTW" in source or \
        "DrainFinalizerSchedulerUnderSTW" in source or \
        "DrainSchedulerUnderSTW" in source:
    raise SystemExit("MarkTermination must not retain phase-specific or ambiguous drain names")
PY

echo 'MSGC MarkTermination lifecycle contract holds'
