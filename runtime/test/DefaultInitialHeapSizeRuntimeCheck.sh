#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 && $# -ne 4 ]]; then
    echo "usage: $0 <sdk-root> <cj-source> <expected-heap-kb> [cj-heap-size]" >&2
    exit 2
fi

SDK_ROOT="$1"
CJ_SOURCE="$2"
EXPECTED_HEAP_KB="$3"
CJ_HEAP_SIZE="${4:-}"

if [[ ! -f "$SDK_ROOT/envsetup.sh" ]]; then
    echo "envsetup.sh not found under SDK root: $SDK_ROOT" >&2
    exit 2
fi

if [[ ! -f "$CJ_SOURCE" ]]; then
    echo "cj source not found: $CJ_SOURCE" >&2
    exit 2
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/cj-default-heap-check.XXXXXX")"
cleanup() {
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

env -i HOME="$HOME" TERM="${TERM:-xterm}" PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin" \
    DYLD_LIBRARY_PATH="" SDK_ROOT="$SDK_ROOT" CJ_SOURCE="$CJ_SOURCE" WORK_DIR="$WORK_DIR" \
    CJ_HEAP_SIZE="$CJ_HEAP_SIZE" \
    bash -c '
        set -eo pipefail
        source "$SDK_ROOT/envsetup.sh" >/dev/null 2>&1
        cd "$WORK_DIR"
        cjc "$CJ_SOURCE" -o default_heap_probe
        if [[ -n "$CJ_HEAP_SIZE" ]]; then
            MRT_REPORT="$WORK_DIR/gc.log" cjHeapSize="$CJ_HEAP_SIZE" \
                ./default_heap_probe > stdout.txt 2> stderr.txt
        else
            MRT_REPORT="$WORK_DIR/gc.log" ./default_heap_probe > stdout.txt 2> stderr.txt
        fi
    '

HEAP_LINE="$(grep -m1 'Heap size:' "$WORK_DIR/gc.log" || true)"
if [[ -z "$HEAP_LINE" ]]; then
    echo "failed to find Heap size line in $WORK_DIR/gc.log" >&2
    echo "--- gc.log ---" >&2
    cat "$WORK_DIR/gc.log" >&2 || true
    exit 1
fi

ACTUAL_HEAP_KB="$(echo "$HEAP_LINE" | sed -E 's/.*Heap size: ([0-9]+)\(KB\).*/\1/')"
if [[ "$ACTUAL_HEAP_KB" != "$EXPECTED_HEAP_KB" ]]; then
    echo "default heap mismatch: expected=${EXPECTED_HEAP_KB}KB actual=${ACTUAL_HEAP_KB}KB" >&2
    echo "$HEAP_LINE" >&2
    exit 1
fi

echo "default heap matches expected value: ${ACTUAL_HEAP_KB}KB"
