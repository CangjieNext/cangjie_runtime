#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RUNNER="$ROOT_DIR/test/MSMarkReadyShardRuntimeSmoke.sh"

if [[ ! -f "$RUNNER" ]]; then
    echo "missing mark-ready shard runtime smoke runner: $RUNNER" >&2
    exit 1
fi

rg -q -- 'cjc .*--gc-mode=marksweep' "$RUNNER"

echo 'MSMarkReadyShard runtime smoke always compiles for marksweep'
