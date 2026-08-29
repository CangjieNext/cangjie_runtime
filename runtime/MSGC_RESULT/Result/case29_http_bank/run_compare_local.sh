#!/usr/bin/env bash
set -uo pipefail

# Linux/aarch64 local comparison: Java 25, local MSGC SDK, and local CopyGC SDK.
CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME_ROOT="$(cd "$CASE_DIR/../../.." && pwd)"
WORKSPACE_ROOT="$(cd "$RUNTIME_ROOT/../.." && pwd)"
MSGC_SDK_ROOT="${MSGC_SDK_ROOT:-$WORKSPACE_ROOT/cangjie_compiler_CMSGC/output/msgc}"
COPYGC_SDK_ROOT="${COPYGC_SDK_ROOT:-$WORKSPACE_ROOT/cangjie_compiler/output}"
JAVA_BIN="${JAVA_BIN:-/usr/lib/jvm/java-25-openjdk-arm64/bin/java}"
JAVAC_BIN="${JAVAC_BIN:-/usr/lib/jvm/java-25-openjdk-arm64/bin/javac}"

for required in "$MSGC_SDK_ROOT/envsetup.sh" "$COPYGC_SDK_ROOT/envsetup.sh" "$JAVA_BIN" "$JAVAC_BIN"; do
    [[ -f "$required" ]] || { echo "[ERROR] missing required artifact: $required" >&2; exit 2; }
done

export CASE29_SEED="${CASE29_SEED:-0}"
export CASE29_ACCOUNT_COUNT="${CASE29_ACCOUNT_COUNT:-1000}"
export CASE29_SUBJECT_ID="${CASE29_SUBJECT_ID:-subject-A}"
export CASE29_LIMIT="${CASE29_LIMIT:-50}"
export CASE29_OFFSET="${CASE29_OFFSET:-0}"
export CASE29_CONCURRENCY="${CASE29_CONCURRENCY:-8}"
export CASE29_DURATION_MS="${CASE29_DURATION_MS:-60000}"
export HEAP_SIZE="${HEAP_SIZE:-512mb}"

stamp="$(date +%Y%m%d_%H%M%S)"
RESULT_DIR="${CASE29_RESULT_DIR:-$CASE_DIR/result/${stamp}_local_compare}"
mkdir -p "$RESULT_DIR"
printf 'result_dir=%s\njava_bin=%s\nmsgc_sdk_root=%s\ncopygc_sdk_root=%s\n' \
    "$RESULT_DIR" "$JAVA_BIN" "$MSGC_SDK_ROOT" "$COPYGC_SDK_ROOT" | tee "$RESULT_DIR/run_metadata.env"
env | rg '^(CASE29_|HEAP_SIZE=|PROCS=)' | sort >> "$RESULT_DIR/run_metadata.env"

sample_process() {
    local pid="$1" output="$2"
    printf 'timestamp,rss_kb,cpu_percent\n' >"$output"
    while kill -0 "$pid" 2>/dev/null; do
        local sample
        sample="$(ps -o rss= -o %cpu= -p "$pid" 2>/dev/null | awk 'NF >= 2 {print $1 "," $2}')"
        [[ -z "$sample" ]] || printf '%s,%s\n' "$(date --iso-8601=seconds)" "$sample" >>"$output"
        sleep 0.2
    done
}

run_java() {
    local out="$RESULT_DIR/java" heap_mb="${HEAP_SIZE,,}"
    heap_mb="${heap_mb%b}"
    mkdir -p "$out/classes"
    cp "$CASE_DIR/java_benchmark/Case29MemberAccountsBenchmark.java" "$out/"
    cp "$CASE_DIR/java_benchmark/Case29MemberAccountsBenchmarkTest.java" "$out/"
    "$JAVAC_BIN" --add-modules jdk.httpserver -d "$out/classes" \
        "$out/Case29MemberAccountsBenchmark.java" "$out/Case29MemberAccountsBenchmarkTest.java" \
        >"$out/compile.stdout.txt" 2>"$out/compile.stderr.txt"
    echo "$?" >"$out/compile.exitcode"
    [[ "$(<"$out/compile.exitcode")" == 0 ]] || return 1
    "$JAVA_BIN" --add-modules jdk.httpserver -cp "$out/classes" Case29MemberAccountsBenchmarkTest \
        >"$out/test.stdout.txt" 2>"$out/test.stderr.txt"
    echo "$?" >"$out/test.exitcode"
    [[ "$(<"$out/test.exitcode")" == 0 ]] || return 1
    CASE29_SERVER_PORT=18229 "$JAVA_BIN" --add-modules jdk.httpserver "-Xmx$heap_mb" \
        "-Xlog:gc*:file=$out/gc.log:time,uptime,level,tags" -cp "$out/classes" Case29MemberAccountsBenchmark \
        >"$out/run.stdout.txt" 2>"$out/run.stderr.txt" &
    local pid=$! sampler
    sample_process "$pid" "$out/rss.csv" & sampler=$!
    wait "$pid"; local rc=$?
    kill "$sampler" 2>/dev/null || true; wait "$sampler" 2>/dev/null || true
    echo "$rc" >"$out/run.exitcode"
    [[ "$rc" == 0 ]]
}

run_cangjie() {
    local label="$1" sdk="$2" mode="$3" port="$4"
    local out="$RESULT_DIR/$label"
    local stdx="$sdk/linux_aarch64_cjnative/dynamic/stdx"
    mkdir -p "$out"
    for required in "$stdx/libstdx.net.http.so" "$stdx/libstdx.encoding.url.so"; do
        [[ -f "$required" ]] || { echo "[ERROR] $label missing: $required" >"$out/error.txt"; return 1; }
    done
    cp "$CASE_DIR/cangjie_benchmark/member_accounts_benchmark.cj" "$out/"
    cp "$CASE_DIR/cangjie_benchmark/member_accounts_common.cj" "$out/"
    (
        source "$sdk/envsetup.sh"
        export LD_LIBRARY_PATH="$stdx${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        export cjHeapSize="$HEAP_SIZE"
        [[ -z "${PROCS:-}" ]] || export cjProcessorNum="$PROCS"
        cd "$out"
        cjc_args=(-O2 member_accounts_benchmark.cj member_accounts_common.cj)
        [[ -z "$mode" ]] || cjc_args+=("--gc-mode=$mode")
        cjc_args+=(--import-path "$stdx" -L "$stdx"
            --link-option "$stdx/libstdx.net.http.so" --link-option "$stdx/libstdx.encoding.url.so"
            -o benchmark)
        cjc "${cjc_args[@]}" >compile.stdout.txt 2>compile.stderr.txt
        echo "$?" >compile.exitcode
        [[ "$(<compile.exitcode)" == 0 ]] || exit 1
        CASE29_SERVER_PORT="$port" MRT_REPORT="$out/gc.log" ./benchmark >run.stdout.txt 2>run.stderr.txt &
        bench_pid=$!; sample_process "$bench_pid" rss.csv & sampler=$!
        wait "$bench_pid"; rc=$?
        kill "$sampler" 2>/dev/null || true; wait "$sampler" 2>/dev/null || true
        echo "$rc" >run.exitcode
        [[ "$rc" == 0 ]]
    )
}

status=0
run_java || status=1
run_cangjie msgc "$MSGC_SDK_ROOT" marksweep 18230 || status=1
run_cangjie copygc "$COPYGC_SDK_ROOT" "" 18231 || status=1
for side in java msgc copygc; do
    echo "--- $side ---"
    tail -2 "$RESULT_DIR/$side/run.stdout.txt" 2>/dev/null || true
done | tee "$RESULT_DIR/summary.txt"
python3 "$CASE_DIR/script/compare_results_local.py" "$RESULT_DIR"
echo "[DONE] compare result: $RESULT_DIR"
exit "$status"
