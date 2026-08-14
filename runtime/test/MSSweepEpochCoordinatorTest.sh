#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_BINARY="${TMPDIR:-/tmp}/ms_sweep_epoch_coordinator_test.$$"
trap 'rm -f "$TEST_BINARY"' EXIT
clang++ -std=c++14 -DMRT_USE_MARK_SWEEP -I"$ROOT_DIR" -I"$ROOT_DIR/src" "$ROOT_DIR/test/MSSweepEpochCoordinatorTest.cpp" "$ROOT_DIR/src/Heap/MAllocator/MSSweepEpochCoordinator.cpp" -o "$TEST_BINARY"
"$TEST_BINARY"
