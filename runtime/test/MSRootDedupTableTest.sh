#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_BINARY="${TMPDIR:-/tmp}/ms_root_dedup_table_test.$$"
trap 'rm -f "$TEST_BINARY"' EXIT

clang++ -std=c++14 -DMRT_USE_MARK_SWEEP -I"$ROOT_DIR" \
    "$ROOT_DIR/test/MSRootDedupTableTest.cpp" \
    -o "$TEST_BINARY"
"$TEST_BINARY"

echo 'MSGC root dedup table contract holds'
