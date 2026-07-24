#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_SOURCE="$ROOT_DIR/test/MSProcessorScalePolicyTest.cpp"
POLICY_HEADER="$ROOT_DIR/src/Concurrency/ProcessorScalePolicy.h"
SCHEDULER_SOURCE="$ROOT_DIR/src/CjScheduler.cpp"
TEST_BINARY="${TMPDIR:-/tmp}/ms_processor_scale_policy.$$"
trap 'rm -f "$TEST_BINARY"' EXIT

if [[ ! -f "$POLICY_HEADER" ]]; then
    echo "missing ProcessorScalePolicy.h" >&2
    exit 1
fi

clang++ -std=c++14 -I"$ROOT_DIR" -I"$ROOT_DIR/src" \
    "$TEST_SOURCE" -o "$TEST_BINARY"
"$TEST_BINARY"

python3 - "$SCHEDULER_SOURCE" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()

required = (
    '#include "Concurrency/ProcessorScalePolicy.h"',
    'ResolveProcessorLimit(cpus, true)',
    'ResolveProcessorLimit(cpus, false)',
)
for token in required:
    if token not in source:
        raise SystemExit(f"CjScheduler.cpp does not select the GC-specific Processor limit: {token}")

if "cpus * maxMultiple" in source:
    raise SystemExit("CjScheduler.cpp must not retain the unchecked local CPU multiplier")
PY

echo "MSGC Processor scale contract holds"
