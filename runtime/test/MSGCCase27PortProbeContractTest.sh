#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RUNNER="$ROOT_DIR/MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/run_msgc_high_p.sh"

if [[ ! -f "$RUNNER" ]]; then
    echo "case27 high-P runner not found: $RUNNER" >&2
    exit 1
fi

if rg -q 'lsof [^|]*[|][[:space:]]*grep -q' "$RUNNER"; then
    echo "port probing must not use an early-closing pipeline under pipefail" >&2
    exit 1
fi

if ! rg -q 'lsof -nP -t -iTCP:"[$]SERVER_PORT" -sTCP:LISTEN >/dev/null 2>&1' "$RUNNER"; then
    echo "port probing must use a direct LISTEN pid query" >&2
    exit 1
fi

echo "case27 high-P port probe is pipefail-safe"
