#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

for symbol in ResetCycleCompletedMarkWork AccountCycleCompletedMarkWork GetCycleCompletedMarkWork; do
    if ! rg -q "$symbol" "$HEADER" "$SOURCE"; then
        echo "MSGC Mark work accounting must expose $symbol" >&2
        exit 1
    fi
done

if ! rg -Uq 'void MarkSweepCollector::AccountCycleCompletedMarkWork\(uint64_t completedWU\)[[:space:][:print:]]*cycleCompletedMarkWU_\.fetch_add' "$SOURCE"; then
    echo "MSGC Mark work must merge through one accounting gateway" >&2
    exit 1
fi

python3 - "$SOURCE" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
needle = 'cycleCompletedMarkWU_.fetch_add'
start = source.index('void MarkSweepCollector::AccountCycleCompletedMarkWork')
end = source.index('\n}', start) + 2
if source.count(needle) != 1 or needle not in source[start:end]:
    raise SystemExit('MSGC Mark work must not bypass the accounting gateway')
PY

echo "MSGC Mark cycle accounting contract holds"
