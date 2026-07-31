#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
ALLOC_BUFFER="$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"

prepare_block="$(
    sed -n \
        '/void MarkSweepCollector::PrepareBarrierMarkBuffersForNewEpochUnderSTW/,/^}/p' \
        "$COLLECTOR"
)"

if ! rg -q 'ResetMSBarrierMarkBuffer\(markBuffer\)' <<<"$prepare_block"; then
    echo 'PreEnum must reset every owner buffer before publishing a new mark epoch' >&2
    exit 1
fi

if rg -q 'CHECK|FlushBarrierMarkBuffer' <<<"$prepare_block"; then
    echo 'PreEnum must discard prior-epoch buffer state instead of asserting or publishing it into the new epoch' >&2
    exit 1
fi

fini_block="$(
    sed -n '/void AllocBufferMS::Fini()/,/^}/p' "$ALLOC_BUFFER"
)"

if ! rg -q 'ResetMSBarrierMarkBuffer\(barrierMarkBuffer_\)' <<<"$fini_block"; then
    echo 'AllocBufferMS exit outside an active mark epoch must discard stale barrier state' >&2
    exit 1
fi

if rg -q 'finalized with pending barrier work' <<<"$fini_block"; then
    echo 'AllocBufferMS exit must not abort on prior-epoch barrier state' >&2
    exit 1
fi

python3 - "$COLLECTOR" <<'PY'
import pathlib
import sys

collector = pathlib.Path(sys.argv[1]).read_text()
start = collector.index("void MarkSweepCollector::PreEnumRoots()")
end = collector.index("void MarkSweepCollector::EnumRoots()", start)
pre_enum = collector[start:end]

reset = pre_enum.index("markScheduler_.ResetCycle()")
activate = pre_enum.index("activeCollector_.store(this")
assert reset < activate, (
    "PreEnum must keep activeCollector closed until ResetCycle has discarded "
    "all prior-epoch scheduler and barrier state"
)
PY

echo 'MSGC barrier epoch-reset contract holds'
