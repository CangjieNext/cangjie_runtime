#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Mutator/MutatorManager.h"
SOURCE="$ROOT_DIR/src/Mutator/MutatorManager.cpp"

python3 - "$HEADER" "$SOURCE" <<'PY'
import pathlib
import sys

header = pathlib.Path(sys.argv[1]).read_text()
source = pathlib.Path(sys.argv[2]).read_text()

for declaration in (
    "void TransitionAllMutatorsToMarkSweepGCPhase(GCPhase phase);",
    "void TransitionAllMutatorsToCopyGCPhase(GCPhase phase);",
):
    assert declaration in header, f"missing separate GC-phase transition API: {declaration}"

dispatch_start = source.index("void MutatorManager::TransitionAllMutatorsToGCPhase")
dispatch_end = source.index("void MutatorManager::TransitionAllMutatorsToMarkSweepGCPhase", dispatch_start)
dispatch = source[dispatch_start:dispatch_end]
assert "TransitionAllMutatorsToMarkSweepGCPhase(phase);" in dispatch
assert "TransitionAllMutatorsToCopyGCPhase(phase);" in dispatch
assert "InstallBarrier" not in dispatch
assert "EnsurePhaseTransition" not in dispatch

ms_start = source.index("void MutatorManager::TransitionAllMutatorsToMarkSweepGCPhase", dispatch_end)
ms_end = source.index("bool MutatorManager::HasPendingEnumAssist", ms_start)
mark_sweep = source[ms_start:ms_end]
assert "enumAssistContext_" in mark_sweep
assert "IsMSEnumAssistEnabled" in mark_sweep
assert "EnumRoots handshake:" not in mark_sweep

copy_start = source.index("void MutatorManager::TransitionAllMutatorsToCopyGCPhase")
copy_end = source.index("#endif\n\n#ifdef MRT_USE_MARK_SWEEP\nbool MutatorManager::HasPendingEnumAssist", copy_start)
copy_gc = source[copy_start:copy_end]
assert "enumAssistContext_" not in copy_gc
assert "EnumRoots handshake:" not in copy_gc
assert "InstallBarrier(phase)" in copy_gc
assert "EnsurePhaseTransition(phase, undoneMutators)" in copy_gc

print("Mutator GC phase transition split contract holds")
PY
