#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT_DIR/../../cangjie/envsetup.sh"

OUT_DIR="$(mktemp -d /tmp/c2r-unwind-validation.XXXXXX)"
trap 'rm -rf "$OUT_DIR"' EXIT

TEST_SRC="$ROOT_DIR/tests/c2r_unwind_exception_validation.cj"
TEST_BIN="$OUT_DIR/c2r_unwind_exception_validation"
RUNTIME_LIB_DIR="$CANGJIE_HOME/runtime/lib/linux_x86_64_cjnative"

cjc "$TEST_SRC" -o "$TEST_BIN"
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:${LD_LIBRARY_PATH:-}" "$TEST_BIN"
