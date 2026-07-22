#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MBarrier/WriteBarrierBuffer.h"

if [[ -e "$HEADER" ]]; then
    echo "obsolete WriteBarrierBuffer header still exists: $HEADER" >&2
    exit 1
fi

if rg -n '\bWriteBarrierBuffer\b|kCreditFlushThreshold' "$ROOT_DIR/src"; then
    echo "obsolete write-barrier buffer declarations or tuning constants remain" >&2
    exit 1
fi

echo "MSGC obsolete write-barrier buffer cleanup contract holds"
