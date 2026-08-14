#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_BINARY="$(mktemp "${TMPDIR:-/tmp}/ms_assist_pacing_controller.XXXXXX")"
trap 'rm -f "$TEST_BINARY"' EXIT

clang++ -std=c++14 -DMRT_USE_MARK_SWEEP \
    -I"$ROOT_DIR" -I"$ROOT_DIR/src" \
    "$ROOT_DIR/test/MSAssistPacingControllerTest.cpp" \
    -o "$TEST_BINARY"
"$TEST_BINARY"
