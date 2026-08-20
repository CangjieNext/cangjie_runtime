#!/usr/bin/env bash
set -euo pipefail
CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$CASE_DIR/script/run_case29_compare.py"
