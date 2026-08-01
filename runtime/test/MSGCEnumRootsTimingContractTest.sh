#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
MUTATOR_MANAGER="$ROOT_DIR/src/Mutator/MutatorManager.cpp"

python3 - "$COLLECTOR" "$MUTATOR_MANAGER" <<'PY'
import pathlib
import sys

collector = pathlib.Path(sys.argv[1]).read_text()
mutator_manager = pathlib.Path(sys.argv[2]).read_text()

enum_start = collector.index("void MarkSweepCollector::EnumRoots()")
enum_end = collector.index("bool MarkSweepCollector::MarkObject", enum_start)
enum_roots = collector[enum_start:enum_end]

for token in (
    "enumMutatorCount",
    "mutatorCountUs",
    "phaseTransitionUs",
    "rootEnumerationUs",
    "rootPublishUs",
    "[MS-GC] EnumRoots: stage-times",
):
    assert token in enum_roots, f"EnumRoots timing breakdown is missing: {token}"

assert enum_roots.index("TransitionAllMutatorsToGCPhase") < enum_roots.index("phaseTransitionUs"), (
    "phaseTransitionUs must measure the MarkSetup handshake"
)
assert enum_roots.index("EnumAndMarkRoots") < enum_roots.index("rootEnumerationUs"), (
    "rootEnumerationUs must measure root enumeration"
)
assert enum_roots.index("PublishRootStaging") < enum_roots.index("rootPublishUs"), (
    "rootPublishUs must measure staged-root publication"
)

transition_start = mutator_manager.index("void MutatorManager::TransitionAllMutatorsToGCPhase")
transition_end = mutator_manager.index("void MutatorManager::EnsureCpuProfileFinish", transition_start)
transition = mutator_manager[transition_start:transition_end]
for token in (
    "enumHandshakeInstallUs",
    "enumHandshakeBroadcastUs",
    "enumHandshakeWaitUs",
    "enumHandshakeAssistUs",
    "assistDrain=",
    "[MS-GC] EnumRoots handshake:",
):
    assert token in transition, f"MarkSetup handshake timing is missing: {token}"

print("MSGC EnumRoots timing contract holds")
PY
