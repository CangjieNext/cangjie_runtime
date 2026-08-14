#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_BINARY="${TMPDIR:-/tmp}/msgc_config_resolver_test.$$"
trap 'rm -f "$TEST_BINARY"' EXIT
clang++ -std=c++14 -DMRT_USE_MARK_SWEEP -I"$ROOT_DIR" -I"$ROOT_DIR/src" "$ROOT_DIR/test/MSGCConfigResolverTest.cpp" -o "$TEST_BINARY"
"$TEST_BINARY"
