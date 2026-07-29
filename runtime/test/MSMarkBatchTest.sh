#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_BINARY="${TMPDIR:-/tmp}/ms_mark_batch_test.$$"
trap 'rm -f "$TEST_BINARY"' EXIT

clang++ -std=c++14 -DMRT_USE_MARK_SWEEP \
    -I"$ROOT_DIR" -I"$ROOT_DIR/src" \
    -I"$ROOT_DIR/third_party/third_party_bounds_checking_function/include" \
    "$ROOT_DIR/test/MSMarkBatchTest.cpp" \
    -o "$TEST_BINARY"
"$TEST_BINARY"

echo 'MSGC MarkWordBatch contract holds'
