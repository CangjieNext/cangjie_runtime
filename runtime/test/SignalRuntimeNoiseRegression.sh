#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CASE_DIR="$ROOT_DIR/MSGC_RESULT/Result/case28_exception_ffi_boundary"
SDK_ROOT="${SDK_ROOT:-/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260619/cangjie}"
WORK_DIR="${WORK_DIR:-$CASE_DIR/result_signal_noise_regression}"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

env -i HOME="$HOME" TERM="${TERM:-xterm-256color}" PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin" \
  SDK_ROOT="$SDK_ROOT" CASE_DIR="$CASE_DIR" WORK_DIR="$WORK_DIR" \
  bash -c '
    set -eo pipefail
    source "$SDK_ROOT/envsetup.sh" >/dev/null 2>&1
    cd "$WORK_DIR"
    clang -c "$CASE_DIR/native.c" -o native.o -mmacosx-version-min=12.0 -fstack-protector-strong
    ar rcs libcase28native.a native.o
    cjc "$CASE_DIR/native_explicit_rethrow.cj" -L . -l case28native -o case28_native_explicit_rethrow
    set +e
    ./case28_native_explicit_rethrow >case28_native_explicit_rethrow.stdout 2>case28_native_explicit_rethrow.stderr
    rc=$?
    set -e
    echo "$rc" > case28_native_explicit_rethrow.rc
  '

STDERR_FILE="$WORK_DIR/case28_native_explicit_rethrow.stderr"
RC="$(cat "$WORK_DIR/case28_native_explicit_rethrow.rc")"
SIG_LINES="$(grep -c "catched unhandled SIGSEGV" "$STDERR_FILE" || true)"
RUNTIME_LINES="$(grep -c "from runtime frame" "$STDERR_FILE" || true)"

echo "exit_code=$RC"
echo "sigsegv_lines=$SIG_LINES"
echo "runtime_frame_lines=$RUNTIME_LINES"

if [ "$RC" -eq 0 ]; then
  echo "expected native_explicit_rethrow to fail, but it exited 0" >&2
  exit 1
fi

if [ "$SIG_LINES" -gt 4 ]; then
  echo "too many repeated SIGSEGV lines: $SIG_LINES" >&2
  sed -n '1,80p' "$STDERR_FILE" >&2
  exit 1
fi

if [ "$RUNTIME_LINES" -gt 1 ]; then
  echo "too many repeated runtime-frame lines: $RUNTIME_LINES" >&2
  sed -n '1,80p' "$STDERR_FILE" >&2
  exit 1
fi

sed -n '1,40p' "$STDERR_FILE"
