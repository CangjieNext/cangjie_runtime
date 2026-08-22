#!/usr/bin/env bash
set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
MSGC_SDK_ROOT="${MSGC_SDK_ROOT:-/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc}"
MSGC_STDX_ROOT="${MSGC_STDX_ROOT:-$MSGC_SDK_ROOT/darwin_aarch64_cjnative}"
STDX_DIR="$MSGC_STDX_ROOT/dynamic/stdx"
PAYLOAD="${CASE25_PAYLOAD:-10K}"
CONCURRENCY="${CASE25_CONCURRENCY:-8}"
DURATION_MS="${CASE25_DURATION_MS:-60000}"
HEAP_SIZE="${HEAP_SIZE:-2gb}"
PORT="${CASE25_SERVER_PORT:-18225}"
TS="$(date +%Y%m%d)"
PAYLOAD_TAG="$(printf '%s' "$PAYLOAD" | tr '[:upper:]' '[:lower:]')"
OUT_BASE="$CASE_DIR/result/${TS}_${HEAP_SIZE}_c${CONCURRENCY}_${PAYLOAD_TAG}_stream_msgc"
OUT_DIR="$OUT_BASE"
SUFFIX=1
while [[ -e "$OUT_DIR" ]]; do
    OUT_DIR="${OUT_BASE}_${SUFFIX}"
    SUFFIX=$((SUFFIX + 1))
done
mkdir -p "$OUT_DIR"

if [[ ! -f "$CASE_DIR/payloads/${PAYLOAD}.json" ]] &&
   [[ -z "${CASE25_PAYLOAD_DIR:-}" ]]; then
    echo "[ERROR] case25 payload not found: ${PAYLOAD}" >&2
    exit 1
fi

# envsetup.sh expands DYLD_LIBRARY_PATH directly on macOS, so it must be
# sourced with nounset disabled when the variable has not been set yet.
set +u
source "$MSGC_SDK_ROOT/envsetup.sh"
set -u
cjc -O2 --gc-mode=marksweep \
    --import-path "$STDX_DIR" -L "$STDX_DIR" \
    --link-option "$STDX_DIR/libstdx.net.http.dylib" \
    --link-option "$STDX_DIR/libstdx.encoding.json.dylib" \
    --link-option "$STDX_DIR/libstdx.encoding.json.stream.dylib" \
    "$CASE_DIR/benchmark.cj" "$CASE_DIR/msgc/stream_transform.cj" "$CASE_DIR/persistent_baseline.cj" \
    -o "$OUT_DIR/benchmark"

echo "[INFO] output: $OUT_DIR"
MRT_REPORT="$OUT_DIR/gc.log" \
cjHeapSize="$HEAP_SIZE" \
CASE25_PAYLOAD="$PAYLOAD" \
CASE25_CONCURRENCY="$CONCURRENCY" \
CASE25_DURATION_MS="$DURATION_MS" \
CASE25_SERVER_PORT="$PORT" \
CASE25_PAYLOAD_DIR="${CASE25_PAYLOAD_DIR:-$CASE_DIR/payloads}" \
DYLD_LIBRARY_PATH="$STDX_DIR:${DYLD_LIBRARY_PATH:-}" \
"$OUT_DIR/benchmark" | tee "$OUT_DIR/stdout.txt"
