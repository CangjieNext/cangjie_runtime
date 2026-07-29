#!/bin/bash
set -e

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
CXX_BIN=${CXX:-c++}
OUTPUT_BIN=$(mktemp "${TMPDIR:-/tmp}/ms-size-class-desc.XXXXXX")
trap 'rm -f "$OUTPUT_BIN"' EXIT

"$CXX_BIN" -std=c++14 -DMRT_USE_MARK_SWEEP \
    -I"$ROOT_DIR/src" \
    -I"$ROOT_DIR/third_party/third_party_bounds_checking_function/include" \
    "$ROOT_DIR/test/MSSizeClassDescTest.cpp" \
    -o "$OUTPUT_BIN"
"$OUTPUT_BIN"
