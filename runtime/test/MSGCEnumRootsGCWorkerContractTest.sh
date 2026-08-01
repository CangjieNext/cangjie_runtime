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
controller = (root / "src/Heap/Collector/MSGCWorkerPoolController.h").read_text()
resources_h = (root / "src/Heap/Collector/CollectorResources.h").read_text()
resources = (root / "src/Heap/Collector/CollectorResources.cpp").read_text()
scheduler = (root / "src/CjScheduler.cpp").read_text()
session = (root / "src/Heap/MCollector/MSEnumRootsWorkerSession.cpp").read_text()
collector = (root / "src/Heap/MCollector/MarkSweepCollector.cpp").read_text()
enum_impl = manager + session + collector
enum_roots = collector[
    collector.index("void MarkSweepCollector::EnumRoots()"):
    collector.index("bool MarkSweepCollector::MarkObject")
]

for token in ("ENUM_ROOTS", "AcquireEnumRoots"):
    assert token in controller, f"worker-pool controller missing {token}"
assert "AcquireMSGCWorkerPoolForEnumRoots" in resources_h
assert "AcquireMSGCWorkerPoolForEnumRoots" in resources

for token in (
    "GCThreadPool",
    "GetMSGCThreadPoolHelperLimit",
    "AcquireMSGCWorkerPoolForEnumRoots",
    "MSGCWorkerPoolOwner::ENUM_ROOTS",
    "TransitionGCPhase(false)",
    "WaitFinish",
    "gcMainScanned",
    "gcWorkerScanned",
    "FinalizeEnumAssistContext",
    "ComputeMSEnumRootsHelperTarget",
    "initialMutatorCount",
    "ClaimMSEnumTransitionBatch",
    "CompleteMSEnumTransitionBatch",
):
    assert token in enum_impl, f"MARK_SETUP transition implementation missing {token}"

assert "GCPhase::GC_PHASE_MS_MARK_SETUP, &enumRootsWorkerSession_" in enum_roots, \
    "MARK_SETUP transition must receive the already-started GC helper session"
assert "nullptr" not in enum_roots[
    enum_roots.index("TransitionAllMutatorsToGCPhase"):
    enum_roots.index("EnumAndMarkRoots")
], "MARK_SETUP transition unexpectedly disables the GC helper session"

assert "ComputeMSEnumRootsHelperTarget" in worker_policy, \
    "EnumRoots initial-mutator helper policy is missing"

for token in (
    "HasPendingEnumAssist",
    "AssistEnumRoots",
    "FinalizeEnumAssistContext",
    "EnumTransitionContext",
):
    assert token in manager_h, f"MutatorManager.h missing enum-assist contract {token}"

for token in ("HandleEnumAssist", "SUSPENSION_FOR_GC_ENUM_ASSIST"):
    assert token in mutator_h, f"Mutator.h missing enum-assist contract {token}"
    assert token in mutator, f"Mutator.cpp missing enum-assist implementation {token}"

assert "cjMSEnumAssistBatchSize" in scheduler, "enum-assist batch-size setting is not read"
assert "TransitionGCPhase(true)" in mutator, "mutator self-scanning transition must remain"

transition = manager[manager.index("void MutatorManager::TransitionAllMutatorsToGCPhase"):]
assert transition.index("EnsureMSMarkSetupPhaseTransition") < transition.index("FinalizeEnumAssistContext"), \
    "MARK_SETUP must drain GC-pool scanning before closing and draining enum assist"

assert "std::unique_ptr<EnumTransitionEntry[]> entries" in manager_h, \
    "transition and assist must share one reusable contiguous entry table"
assert "std::atomic<EnumTransitionState> state" in manager_h, \
    "shared transition entries must use CAS ownership"
assert "std::list<Mutator*>& undoneMutators" not in manager[manager.index("void MutatorManager::EnsureMSMarkSetupPhaseTransition"):manager.index("void MutatorManager::TransitionAllMutatorsToGCPhase")], \
    "MS MARK_SETUP must not keep a second linked pending list"

print("MSGC transition uses GC main + helpers + mutator assist in one pool epoch")
PY
