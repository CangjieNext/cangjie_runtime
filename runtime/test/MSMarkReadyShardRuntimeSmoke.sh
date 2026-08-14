#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <region-mark-msgc-sdk-root>" >&2
    exit 2
fi

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SDK_ROOT="$1"
SOURCE="$ROOT_DIR/test/MSMarkReadyShardRuntimeSmoke.cj"

if [[ ! -f "$SDK_ROOT/envsetup.sh" ]]; then
    echo "envsetup.sh not found under SDK root: $SDK_ROOT" >&2
    exit 2
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ms-mark-ready-shard-smoke.XXXXXX")"
cleanup() {
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

export DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}"
source "$SDK_ROOT/envsetup.sh" >/dev/null 2>&1
cjc "$SOURCE" --gc-mode=marksweep -O2 -o "$WORK_DIR/smoke"

MRT_REPORT="$WORK_DIR/gc.log" cjGCThreads="${cjGCThreads:-8}" \
    "$WORK_DIR/smoke" >"$WORK_DIR/stdout.txt" 2>"$WORK_DIR/stderr.txt"

rg -q 'MSMarkReadyShardRuntimeSmoke PASS' "$WORK_DIR/stdout.txt"
rg -q '\[MS-GC\] Mark ready shards: .*shards=' "$WORK_DIR/gc.log"
rg -q '\[MS-GC\] --- ConcurrentMark begin ---' "$WORK_DIR/gc.log"
rg -q '\[MS-GC\] --- ConcurrentMark end: total=[0-9]+us ---' "$WORK_DIR/gc.log"

cat "$WORK_DIR/stdout.txt"
rg '\[MS-GC\] Mark ready shards:|--- ConcurrentMark (begin|end):' "$WORK_DIR/gc.log"
