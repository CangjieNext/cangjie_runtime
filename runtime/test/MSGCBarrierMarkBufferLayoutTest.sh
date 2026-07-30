#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_SOURCE="$ROOT_DIR/test/MSGCBarrierMarkBufferLayoutTest.cpp"
TEST_BINARY="${TMPDIR:-/tmp}/msgc_barrier_mark_buffer_layout.$$"
trap 'rm -f "$TEST_BINARY"' EXIT

clang++ -std=c++14 -DMRT_USE_MARK_SWEEP -I"$ROOT_DIR" -I"$ROOT_DIR/src" \
    "$TEST_SOURCE" -o "$TEST_BINARY"
"$TEST_BINARY"

echo 'MSGC barrier mark buffer fixed layout contract holds'
