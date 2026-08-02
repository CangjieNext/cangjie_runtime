#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_BINARY="${TMPDIR:-/tmp}/ms_mark_ready_shard_policy_test.$$"
trap 'rm -f "$TEST_BINARY"' EXIT

clang++ -std=c++14 -DMRT_USE_MARK_SWEEP \
    -I"$ROOT_DIR" -I"$ROOT_DIR/src" \
    "$ROOT_DIR/test/MSMarkReadyShardPolicyTest.cpp" \
    -o "$TEST_BINARY"
"$TEST_BINARY"

echo 'MSGC Mark ready shard policy holds'
