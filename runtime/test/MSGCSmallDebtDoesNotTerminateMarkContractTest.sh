#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MARK_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
TERMINATION_POLICY="$ROOT_DIR/src/Heap/MCollector/MSMarkTerminationPolicy.h"

python3 - "$MARK_SOURCE" "$TERMINATION_POLICY" <<'PY'
from pathlib import Path
import sys


mark_source, policy = [
    Path(path).read_text(encoding="utf-8") for path in sys.argv[1:]
]

for label, source in {
    "MarkSweepCollector.cpp": mark_source,
    "MSMarkTerminationPolicy.h": policy,
}.items():
    for forbidden in (
        "DEBT_BUDGET",
        "debt-budget",
        "forceForDebtBudget",
        "kTerminationBudgetBuffers",
    ):
        if forbidden in source:
            raise SystemExit(
                f"{label} must not terminate ConcurrentMark for small debt: {forbidden}"
            )
PY

echo 'MSGC small mark debt continues to natural convergence'
