#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT_DIR" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
manager = (root / "src/Mutator/MutatorManager.cpp").read_text()
manager_h = (root / "src/Mutator/MutatorManager.h").read_text()
worker_policy = (root / "src/Heap/MCollector/MSMarkWorkerPolicy.h").read_text()
mutator = (root / "src/Mutator/Mutator.cpp").read_text()
mutator_h = (root / "src/Mutator/Mutator.h").read_text()
scheduler = (root / "src/CjScheduler.cpp").read_text()
collector = (root / "src/Heap/MCollector/MarkSweepCollector.cpp").read_text()
enum_roots = collector[
    collector.index("void MarkSweepCollector::EnumRoots()"):
    collector.index("bool MarkSweepCollector::MarkObject")
]

controller = (root / "src/Heap/MCollector/MSGCWorkerPoolController.h").read_text()
resources_h = (root / "src/Heap/Collector/CollectorResources.h").read_text()
resources = (root / "src/Heap/Collector/CollectorResources.cpp").read_text()
for token in ("ENUM_ROOTS", "AcquireEnumRoots", "AcquireMSGCWorkerPoolForEnumRoots"):
    assert token not in controller + resources_h + resources, f"obsolete EnumRoots worker-pool path remains: {token}"

assert "enumRootsWorkerSession_" not in enum_roots
assert "ComputeMSEnumRootsHelperTarget" not in enum_roots
assert "TransitionAllMutatorsToGCPhase" in enum_roots

for token in (
    "HasPendingEnumAssist",
    "AssistEnumRoots",
    "FinalizeEnumAssistContext",
    "EnumAssistContext",
):
    assert token in manager_h, f"MutatorManager.h missing enum-assist contract {token}"

for token in ("HandleEnumAssist", "SUSPENSION_FOR_GC_ENUM_ASSIST"):
    assert token in mutator_h, f"Mutator.h missing enum-assist contract {token}"
    assert token in mutator, f"Mutator.cpp missing enum-assist implementation {token}"

assert "cjMSEnumAssistBatchSize" in scheduler, "enum-assist batch-size setting is not read"
assert "TransitionGCPhase(true)" in mutator, "mutator self-scanning transition must remain"

assert "ClaimEnumAssistBatch" in manager
for removed in (
    "UpdateEnumAssistRemaining",
    "gcMainScanned",
    "mutatorAssistScanned",
    "readyPolls",
    "enumHandshakeInstallUs",
):
    assert removed not in manager, f"enum-assist diagnostic state remains: {removed}"
assert "EnsurePhaseTransition(GCPhase phase, std::list<Mutator*> &undoneMutators)" in manager
assert "EnumTransitionEntry" not in manager_h

print("MSGC transition uses GC main plus monotonic mutator assist without a worker-pool epoch")
PY
