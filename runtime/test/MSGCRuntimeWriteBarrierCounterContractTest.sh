#!/usr/bin/env bash
set -euo pipefail

runtime_root="$(cd "$(dirname "$0")/.." && pwd)"
source_file="$runtime_root/src/CompilerCalls.cpp"

python3 - "$source_file" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()

required = {
    "MCC_WriteRefField": "MSGC_RUNTIME_WB_REF",
    "MCC_WriteStaticRef": "MSGC_RUNTIME_WB_STATIC_REF",
    "MCC_WriteStructField": "MSGC_RUNTIME_WB_STRUCT",
    "MCC_WriteStaticStruct": "MSGC_RUNTIME_WB_STATIC_STRUCT",
    "CJ_MCC_ArrayCopyRef": "MSGC_RUNTIME_WB_ARRAY_REF",
    "CJ_MCC_ArrayCopyStruct": "MSGC_RUNTIME_WB_ARRAY_STRUCT",
}

for function, counter in required.items():
    match = re.search(
        rf'extern "C"[^\n]*\b{re.escape(function)}\b.*?(?=\nextern "C"|\Z)',
        source,
        re.S,
    )
    if match is None:
        raise SystemExit(f"missing Runtime barrier helper: {function}")
    if f"CountMSGCRuntimeWriteBarrier({counter})" not in match.group(0):
        raise SystemExit(f"{function} does not increment {counter}")

for token in (
    "thread_local MSGCRuntimeWriteBarrierThreadCounters*",
    "fetch_add(1, std::memory_order_relaxed)",
    "load(std::memory_order_relaxed)",
    "[MSGC_WB_RUNTIME_COUNTS]",
    "array_struct=%",
    "total=%",
):
    if token not in source:
        raise SystemExit(f"missing Runtime barrier counter contract token: {token}")
PY

echo "MSGC Runtime write-barrier counter contract passed"
