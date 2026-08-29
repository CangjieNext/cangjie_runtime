#!/usr/bin/env bash
set -euo pipefail

# Run the MSGC side of case29 using the SDK built in this workspace.
# Override MSGC_SDK_ROOT or MSGC_STDX_DIR only when using another local build.
CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME_ROOT="$(cd "$CASE_DIR/../../.." && pwd)"
WORKSPACE_ROOT="$(cd "$RUNTIME_ROOT/../.." && pwd)"
SDK_ROOT="${MSGC_SDK_ROOT:-$WORKSPACE_ROOT/cangjie_compiler_CMSGC/output/msgc}"
STDX_DIR="${MSGC_STDX_DIR:-$SDK_ROOT/linux_aarch64_cjnative/dynamic/stdx}"

for required in \
    "$SDK_ROOT/envsetup.sh" \
    "$SDK_ROOT/bin/cjc" \
    "$STDX_DIR/libstdx.net.http.so" \
    "$STDX_DIR/libstdx.encoding.url.so"; do
    if [[ ! -f "$required" ]]; then
        echo "[ERROR] required local MSGC artifact is missing: $required" >&2
        exit 2
    fi
done

stamp="$(date +%Y%m%d_%H%M%S)"
RESULT_DIR="${CASE29_RESULT_DIR:-$CASE_DIR/result/${stamp}_msgc_local}"
mkdir -p "$RESULT_DIR"

source "$SDK_ROOT/envsetup.sh"
export LD_LIBRARY_PATH="$STDX_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CASE29_SEED="${CASE29_SEED:-0}"
export CASE29_ACCOUNT_COUNT="${CASE29_ACCOUNT_COUNT:-1000}"
export CASE29_SUBJECT_ID="${CASE29_SUBJECT_ID:-subject-A}"
export CASE29_LIMIT="${CASE29_LIMIT:-50}"
export CASE29_OFFSET="${CASE29_OFFSET:-0}"
export CASE29_CONCURRENCY="${CASE29_CONCURRENCY:-8}"
export CASE29_DURATION_MS="${CASE29_DURATION_MS:-60000}"
export CASE29_SERVER_PORT="${CASE29_SERVER_PORT:-18230}"
export cjHeapSize="${HEAP_SIZE:-512mb}"
if [[ -n "${PROCS:-}" ]]; then
    export cjProcessorNum="$PROCS"
fi

printf 'sdk_root=%s\nstdx_dir=%s\nresult_dir=%s\n' "$SDK_ROOT" "$STDX_DIR" "$RESULT_DIR" \
    | tee "$RESULT_DIR/run_metadata.env"
env | rg '^(CASE29_|cjHeapSize=|cjProcessorNum=)' | sort >> "$RESULT_DIR/run_metadata.env"

cp "$CASE_DIR/cangjie_benchmark/member_accounts_benchmark.cj" "$RESULT_DIR/"
cp "$CASE_DIR/cangjie_benchmark/member_accounts_common.cj" "$RESULT_DIR/"

pushd "$RESULT_DIR" >/dev/null
cjc -O2 member_accounts_benchmark.cj member_accounts_common.cj \
    --gc-mode=marksweep \
    --import-path "$STDX_DIR" -L "$STDX_DIR" \
    --link-option "$STDX_DIR/libstdx.net.http.so" \
    --link-option "$STDX_DIR/libstdx.encoding.url.so" \
    -o benchmark >compile.stdout.txt 2>compile.stderr.txt

MRT_REPORT="$RESULT_DIR/gc.log" ./benchmark >run.stdout.txt 2>run.stderr.txt
popd >/dev/null

echo "[DONE] MSGC case29 result: $RESULT_DIR"
