#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_BINARY="${TMPDIR:-/tmp}/ms_scan_class_test.$$"
trap 'rm -f "$TEST_BINARY"' EXIT

clang++ -std=c++14 -DMRT_USE_MARK_SWEEP \
    -I"$ROOT_DIR" -I"$ROOT_DIR/src" \
    -I"$ROOT_DIR/third_party/third_party_bounds_checking_function/include" \
    "$ROOT_DIR/test/MSScanClassTest.cpp" \
    "$ROOT_DIR/src/Heap/MAllocator/MSScanClass.cpp" \
    -o "$TEST_BINARY"
"$TEST_BINARY"

echo 'MSGC ScanClass contract holds'
