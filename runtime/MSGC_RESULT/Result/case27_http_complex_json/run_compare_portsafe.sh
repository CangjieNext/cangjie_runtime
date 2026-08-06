#!/usr/bin/env bash
# ============================================================
# case27 通用对比脚本：MSGC vs CopyGC (+ Go)
#
# SDK 路径通过环境变量传入，不硬编码。其余参数见 README 中的 case27 约定。
#
#   必传参数（无默认值）：
#     MSGC_SDK_ROOT       MSGC 的 SDK 根路径（应包含 envsetup.sh）
#     MSGC_STDX_ROOT      MSGC 的平台根目录（脚本内部使用 dynamic/stdx）
#     COPYGC_SDK_ROOT     CopyGC 的 SDK 根路径（应包含 envsetup.sh）
#     COPYGC_STDX_ROOT    CopyGC 的平台根目录（脚本内部使用 dynamic/stdx）
#
#   可选环境变量（有默认值）：
#     CASE27_PAYLOAD       1K|10K|100K|1M        默认 10K
#     CASE27_CONCURRENCY   worker 并发数           默认 8
#     CASE27_DURATION_MS   压测时长 (ms)           默认 60000
#     HEAP_SIZE            cjHeapSize             默认 12gb
#     PROCS                可选；显式设置时透传 cjProcessorNum
#     MSGC_HEAP_GROWTH     仅传给 MSGC 的 cjHeapGrowth，默认 1
#     MSGC_SCAVENGE_GROWTH 仅传给 MSGC 的 cjScavengeGrowth，默认 10
#     COPYGC_GC_INTERVAL   仅传给 CopyGC 的 cjGCInterval，默认不设置（如 150ms）
#     MSGC_GC_INTERVAL     仅传给 MSGC 的 cjGCInterval，默认不设置（如 5ms）
#     cjGCBGSweepMaxTime   仅透传给 MSGC 后台 sweep 单 slice 时间预算（如 2ms）
#     CJ_MS_VERIFY_MARK    仅透传给 MSGC runtime 的 verifier 开关，默认不设置
#     CJ_MS_VERIFY_MARK_DETAIL  verifier detail 开关，默认不设置
#     CJ_MS_VERIFY_ROOTS   root 枚举诊断开关，默认不设置
#     CASE27_BODY_DIAG     benchmark 请求/响应体诊断开关，默认不设置
#     CASE27_DUMP_BAD_BODY benchmark 失败请求/响应完整 body 落盘开关，默认不设置
#     CASE27_GRAPH_DIAG    benchmark JSON 图遍历诊断开关，默认不设置
#     CASE27_GRAPH_DIAG_RATE  JSON 图遍历每 N 个节点打印一次，默认不设置
#     CASE27_CLIENT_MODE   sync|async             默认 sync
#     CASE27_SEND_INTERVAL_MS async 发射间隔 (ms)  默认 1
#     CASE27_MAX_INFLIGHT  async 最大在途请求数    默认 32
#     GO_ENABLE            是否加入 Go mirror side 默认 1
#     GO_GODEBUG           Go benchmark 的 GODEBUG 默认 gctrace=1
#     GO_GOGC              可选，透传给 Go side
#
#   用法示例：
#     MSGC_SDK_ROOT=/path/to/msgc/sdk \
#     MSGC_STDX_ROOT=/path/to/msgc/platform-root \
#     COPYGC_SDK_ROOT=/path/to/copygc/sdk \
#     COPYGC_STDX_ROOT=/path/to/copygc/platform-root \
#     CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 \
#     ./run_compare.sh
# ============================================================
set -uo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_DIR="$CASE_DIR/script"
PYTHON_BIN="${PYTHON_BIN:-python3}"
SHARED_MPLCONFIG_DIR="$CASE_DIR/result/.mplconfig"
if [[ ! -f "$SCRIPT_DIR/compare_results.py" ]]; then
    echo "[ERROR] local compare script not found: $SCRIPT_DIR/compare_results.py" >&2
    exit 1
fi

# ---------- SDK 路径：必须通过环境变量传入 ----------
MSGC_SDK="${MSGC_SDK_ROOT:?ERROR: MSGC_SDK_ROOT not set}"
MSGC_STDX_ROOT="${MSGC_STDX_ROOT:?ERROR: MSGC_STDX_ROOT not set}"
COPYGC_SDK="${COPYGC_SDK_ROOT:?ERROR: COPYGC_SDK_ROOT not set}"
COPYGC_STDX_ROOT="${COPYGC_STDX_ROOT:?ERROR: COPYGC_STDX_ROOT not set}"

MSGC_STDX="${MSGC_STDX_ROOT}/dynamic/stdx"
COPYGC_STDX="${COPYGC_STDX_ROOT}/dynamic/stdx"

# ---------- 可选参数（与 README 中的 case27 环境变量对齐） ----------
PAYLOAD="${CASE27_PAYLOAD:-10K}"
CONC="${CASE27_CONCURRENCY:-8}"
DUR="${CASE27_DURATION_MS:-60000}"
HEAP_SIZE="${HEAP_SIZE:-12gb}"
PROCS="${PROCS:-}"
MSGC_HEAP_GROWTH="${MSGC_HEAP_GROWTH:-1}"
MSGC_SCAVENGE_GROWTH="${MSGC_SCAVENGE_GROWTH:-10}"
COPYGC_GC_INTERVAL="${COPYGC_GC_INTERVAL:-}"
MSGC_GC_INTERVAL="${MSGC_GC_INTERVAL:-}"
CLIENT_MODE="${CASE27_CLIENT_MODE:-sync}"
SEND_INTERVAL_MS="${CASE27_SEND_INTERVAL_MS:-1}"
MAX_INFLIGHT="${CASE27_MAX_INFLIGHT:-32}"
GO_ENABLE="${GO_ENABLE:-1}"
GO_GODEBUG="${GO_GODEBUG:-gctrace=1}"
GO_GOGC="${GO_GOGC:-}"

if [[ ! -f "$CASE_DIR/payloads/${PAYLOAD}.json" ]]; then
    echo "[ERROR] payloads/${PAYLOAD}.json not found" >&2
    exit 1
fi

TS="$(date +%Y%m%d)"
PAYLOAD_TAG=$(printf '%s' "$PAYLOAD" | tr '[:upper:]' '[:lower:]')
COMPARE_DIR_BASE="$CASE_DIR/result/${TS}_c${CONC}_${PAYLOAD_TAG}_HeapGrow${MSGC_HEAP_GROWTH}_Scavenge${MSGC_SCAVENGE_GROWTH}_compare"
COMPARE_DIR="$COMPARE_DIR_BASE"
COMPARE_DIR_SUFFIX=1
while [[ -e "$COMPARE_DIR" ]]; do
    COMPARE_DIR="${COMPARE_DIR_BASE}_${COMPARE_DIR_SUFFIX}"
    COMPARE_DIR_SUFFIX=$((COMPARE_DIR_SUFFIX + 1))
done
COPYGC_DIR="$COMPARE_DIR/copygc"
MSGC_DIR="$COMPARE_DIR/msgc"
GO_DIR="$COMPARE_DIR/go"
MPLCONFIG_DIR="$SHARED_MPLCONFIG_DIR"
mkdir -p "$COPYGC_DIR" "$MSGC_DIR"
if [[ "$GO_ENABLE" != "0" ]]; then
    mkdir -p "$GO_DIR"
fi
mkdir -p "$MPLCONFIG_DIR"
echo "[INFO] compare dir: $COMPARE_DIR"
echo "[INFO] config: payload=$PAYLOAD concurrency=$CONC duration=${DUR}ms heap=$HEAP_SIZE proc=${PROCS:-auto} mode=$CLIENT_MODE"

run_side() {
    local label="$1"
    local out_dir="$2"
    local sdk_root="$3"
    local stdx_dir="$4"
    local port="$5"
    local runtime_heap_growth="${6:-}"
    local runtime_scavenge_growth="${7:-}"
    local runtime_gc_interval="${8:-}"
    local orig_port=$port
    local max_tries=10
    local tries=0
    while [ $tries -lt $max_tries ]; do
        if ! lsof -nP -iTCP:"$port" 2>/dev/null | grep -q .; then
            break
        fi
        echo "[$label] port $port is in use, trying $((port + 1))"
        port=$((port + 1))
        tries=$((tries + 1))
    done
    if [ "$port" != "$orig_port" ]; then
        echo "[$label] using port $port (original $orig_port was occupied)"
    fi
    if [ $tries -ge $max_tries ]; then
        echo "[$label] ERROR: could not find free port after $max_tries tries" >&2
        return 1
    fi

    echo "[INFO] ===== $label ($sdk_root) ====="
    {
        echo "case=case27_http_complex_json"
        echo "label=$label"
        echo "sdk_root=$sdk_root"
        echo "stdx_dir=$stdx_dir"
        echo "payload=$PAYLOAD"
        echo "concurrency=$CONC"
        echo "duration_ms=$DUR"
        echo "client_timeout_ms=${CASE27_CLIENT_TIMEOUT_MS:-5000}"
        echo "client_mode=$CLIENT_MODE"
        echo "send_interval_ms=$SEND_INTERVAL_MS"
        echo "max_inflight=$MAX_INFLIGHT"
        echo "server_port=$port"
        echo "original_port=$orig_port"
        echo "heap_size=$HEAP_SIZE"
        echo "cj_heap_growth=${runtime_heap_growth:-}"
        echo "cj_scavenge_growth=${runtime_scavenge_growth:-}"
        echo "cj_gc_interval=${runtime_gc_interval:-}"
        echo "procs=${PROCS:-}"
        echo "schdfd_timeout_diag=${CJ_SCHDFD_TIMEOUT_DIAG:-0}"
        echo "cj_ms_disable_assist=${CJ_MS_DISABLE_ASSIST:-}"
        echo "cj_ms_bg_scavenge=${CJ_MS_BG_SCAVENGE:-}"
        echo "cj_ms_pin_hot_free=${CJ_MS_PIN_HOT_FREE:-}"
        echo "cj_ms_mark_time_budget_ms=${CJ_MS_MARK_TIME_BUDGET_MS:-}"
        echo "cj_ms_verify_mark=${CJ_MS_VERIFY_MARK:-}"
        echo "cj_ms_verify_mark_detail=${CJ_MS_VERIFY_MARK_DETAIL:-}"
        echo "cj_ms_verify_roots=${CJ_MS_VERIFY_ROOTS:-}"
        echo "case27_body_diag=${CASE27_BODY_DIAG:-}"
        echo "case27_dump_bad_body=${CASE27_DUMP_BAD_BODY:-}"
        echo "case27_dump_dir=$out_dir"
        echo "case27_graph_diag=${CASE27_GRAPH_DIAG:-}"
        echo "case27_graph_diag_rate=${CASE27_GRAPH_DIAG_RATE:-}"
        echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "runtime_dylib_md5=$(md5 -q "$sdk_root"/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib 2>/dev/null || echo 'N/A')"
    } > "$out_dir/run_metadata.env"
    cp "$CASE_DIR/benchmark.cj" "$out_dir/benchmark.cj.snapshot"
    cp "$CASE_DIR/payloads/${PAYLOAD}.json" "$out_dir/payload.snapshot.json"

    local -a child_env=(
        "HOME=$HOME"
        "TERM=${TERM:-xterm}"
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin"
        "SDK_ROOT=$sdk_root"
        "STDX_DIR=$stdx_dir"
        "OUT_DIR=$out_dir"
        "LABEL=$label"
        "HEAP_SIZE=$HEAP_SIZE"
        "PROCS=$PROCS"
        "RUNTIME_HEAP_GROWTH=$runtime_heap_growth"
        "RUNTIME_SCAVENGE_GROWTH=$runtime_scavenge_growth"
        "RUNTIME_GC_INTERVAL=$runtime_gc_interval"
        "BENCH_SRC=$CASE_DIR/benchmark.cj"
        "PAYLOAD_DIR=$CASE_DIR/payloads"
        "CASE27_PAYLOAD=$PAYLOAD"
        "CASE27_CONCURRENCY=$CONC"
        "CASE27_DURATION_MS=$DUR"
        "CASE27_SERVER_PORT=$port"
        "CASE27_CLIENT_TIMEOUT_MS=${CASE27_CLIENT_TIMEOUT_MS:-5000}"
        "CASE27_PAYLOAD_DIR=$CASE_DIR/payloads"
        "CASE27_DUMP_DIR=$out_dir"
        "CASE27_CLIENT_MODE=$CLIENT_MODE"
        "CASE27_SEND_INTERVAL_MS=$SEND_INTERVAL_MS"
        "CASE27_MAX_INFLIGHT=$MAX_INFLIGHT"
    )
    local optional_var=""
    local optional_value=""
    for optional_var in \
        CASE27_BODY_DIAG \
        CASE27_DUMP_BAD_BODY \
        CASE27_GRAPH_DIAG \
        CASE27_GRAPH_DIAG_RATE \
        CJ_SCHDFD_TIMEOUT_DIAG \
        CJ_MS_DISABLE_ASSIST \
        CJ_MS_ASSIST_RATIO_SCALE \
        CJ_MS_ASSIST_DEBT_TRIGGER \
        CJ_MS_BG_SCAVENGE \
        CJ_MS_PIN_HOT_FREE \
        CJ_MS_MARK_TIME_BUDGET_MS \
        CJ_MS_VERIFY_MARK \
        CJ_MS_VERIFY_MARK_DETAIL \
        CJ_MS_VERIFY_ROOTS \
        cjGCBGSweepMaxTime; do
        optional_value="${!optional_var-}"
        if [[ -n "$optional_value" ]]; then
            child_env+=("${optional_var}=${optional_value}")
        fi
    done

    env -i "${child_env[@]}" bash -c '
            set -o pipefail
            set +u
            source "$SDK_ROOT/envsetup.sh" >/dev/null 2>&1
            set -u
            if ! command -v cjc >/dev/null 2>&1; then
                export PATH="$SDK_ROOT/bin:$SDK_ROOT/tools/bin:$PATH:$HOME/.cjpm/bin"
            fi
            export DYLD_LIBRARY_PATH="$STDX_DIR:${DYLD_LIBRARY_PATH:-}"
            echo "[$LABEL] cjc: $(which cjc)"
            echo "[$LABEL] DYLD_LIBRARY_PATH=$DYLD_LIBRARY_PATH"
            echo "[$LABEL] CJ_MS_DISABLE_ASSIST=${CJ_MS_DISABLE_ASSIST:-<default>}"
            echo "[$LABEL] CJ_MS_VERIFY_MARK=${CJ_MS_VERIFY_MARK:-<default>}"
            echo "[$LABEL] CJ_MS_VERIFY_MARK_DETAIL=${CJ_MS_VERIFY_MARK_DETAIL:-<default>}"
            echo "[$LABEL] CJ_MS_VERIFY_ROOTS=${CJ_MS_VERIFY_ROOTS:-<default>}"
            echo "[$LABEL] cjGCBGSweepMaxTime=${cjGCBGSweepMaxTime:-<default>}"
            echo "[$LABEL] cjScavengeGrowth=${RUNTIME_SCAVENGE_GROWTH:-<default>}"
            echo "[$LABEL] CASE27_BODY_DIAG=${CASE27_BODY_DIAG:-<default>}"
            echo "[$LABEL] CASE27_DUMP_BAD_BODY=${CASE27_DUMP_BAD_BODY:-<default>}"
            echo "[$LABEL] CASE27_DUMP_DIR=${CASE27_DUMP_DIR:-<default>}"
            echo "[$LABEL] CASE27_GRAPH_DIAG=${CASE27_GRAPH_DIAG:-<default>}"
            echo "[$LABEL] CASE27_GRAPH_DIAG_RATE=${CASE27_GRAPH_DIAG_RATE:-<default>}"

            cd "$OUT_DIR"
            cjc -O2 "$BENCH_SRC" \
                --import-path "$STDX_DIR" \
                -L "$STDX_DIR" \
                --link-option "$STDX_DIR/libstdx.net.http.dylib" \
                --link-option "$STDX_DIR/libstdx.encoding.json.dylib" \
                -o benchmark
            if [ ! -x benchmark ]; then
                echo "[$LABEL] compile failed"; exit 1
            fi

            if [ -n "${RUNTIME_GC_INTERVAL:-}" ]; then
                echo "[$LABEL] cjGCInterval=$RUNTIME_GC_INTERVAL"
                export cjGCInterval="$RUNTIME_GC_INTERVAL"
            else
                echo "[$LABEL] cjGCInterval=<default>"
                unset cjGCInterval 2>/dev/null || true
            fi

            if [ -n "${CJ_MS_MARK_TIME_BUDGET_MS:-}" ]; then
                echo "[$LABEL] CJ_MS_MARK_TIME_BUDGET_MS=$CJ_MS_MARK_TIME_BUDGET_MS"
                export CJ_MS_MARK_TIME_BUDGET_MS
            else
                unset CJ_MS_MARK_TIME_BUDGET_MS 2>/dev/null || true
            fi

            if [ -n "${RUNTIME_HEAP_GROWTH:-}" ]; then
                echo "[$LABEL] cjHeapGrowth=$RUNTIME_HEAP_GROWTH"
                export cjHeapGrowth="$RUNTIME_HEAP_GROWTH"
            else
                echo "[$LABEL] cjHeapGrowth=<default>"
                unset cjHeapGrowth 2>/dev/null || true
            fi

            if [ -n "${RUNTIME_SCAVENGE_GROWTH:-}" ]; then
                echo "[$LABEL] cjScavengeGrowth=$RUNTIME_SCAVENGE_GROWTH"
                export cjScavengeGrowth="$RUNTIME_SCAVENGE_GROWTH"
            else
                echo "[$LABEL] cjScavengeGrowth=<default>"
                unset cjScavengeGrowth 2>/dev/null || true
            fi

            if [ -n "${PROCS:-}" ]; then
                export cjProcessorNum="$PROCS"
            else
                unset cjProcessorNum 2>/dev/null || true
            fi
            MRT_REPORT="$OUT_DIR/gc.log" \
            MRT_LOG_FILE_SIZE="200mb" \
            cjHeapSize="$HEAP_SIZE" \
                "$OUT_DIR/benchmark" > "$OUT_DIR/stdout.txt" 2>&1 &
            BENCH_PID=$!

            perl -MTime::HiRes=sleep,time -e "
                my \$pid = \$ARGV[0];
                while (kill 0, \$pid) {
                    my \$line = \`ps -o rss=,%cpu= -p \$pid 2>/dev/null\`;
                    chomp \$line; \$line =~ s/^\s+//;
                    if (length \$line) {
                        my (\$rss, \$cpu) = split /\s+/, \$line, 2;
                        my \$t = time();
                        my @lt = localtime(int(\$t));
                        my \$frac = sprintf(\"%.3f\", \$t - int(\$t));
                        \$frac =~ s/^0//;
                        printf \"%04d-%02d-%02dT%02d:%02d:%02d%s,%s,%s\n\",
                            \$lt[5]+1900,\$lt[4]+1,\$lt[3],\$lt[2],\$lt[1],\$lt[0],\$frac,\$rss,\$cpu;
                    }
                    sleep(0.2);
                }
            " "$BENCH_PID" > "$OUT_DIR/rss.csv" 2>/dev/null &
            SAMPLER_PID=$!
            wait "$BENCH_PID"
            ec=$?
            kill "$SAMPLER_PID" 2>/dev/null || true
            wait "$SAMPLER_PID" 2>/dev/null || true
            echo "[$LABEL] bench exit=$ec"
            exit $ec
        '
    local rc=$?
    echo "[INFO] ===== $label done (rc=$rc) ====="
    return $rc
}

run_go_side() {
    local label="$1"
    local out_dir="$2"
    local port="$3"
    local orig_port=$port
    local max_tries=10
    local tries=0
    while [ $tries -lt $max_tries ]; do
        if ! lsof -nP -iTCP:"$port" 2>/dev/null | grep -q .; then
            break
        fi
        echo "[$label] port $port is in use, trying $((port + 1))"
        port=$((port + 1))
        tries=$((tries + 1))
    done
    if [ "$port" != "$orig_port" ]; then
        echo "[$label] using port $port (original $orig_port was occupied)"
    fi
    if [ $tries -ge $max_tries ]; then
        echo "[$label] ERROR: could not find free port after $max_tries tries" >&2
        return 1
    fi

    echo "[INFO] ===== $label (Go mirror benchmark) ====="
    {
        echo "case=case27_http_complex_json"
        echo "label=$label"
        echo "payload=$PAYLOAD"
        echo "concurrency=$CONC"
        echo "duration_ms=$DUR"
        echo "client_timeout_ms=${CASE27_CLIENT_TIMEOUT_MS:-5000}"
        echo "client_mode=$CLIENT_MODE"
        echo "send_interval_ms=$SEND_INTERVAL_MS"
        echo "max_inflight=$MAX_INFLIGHT"
        echo "server_port=$port"
        echo "original_port=$orig_port"
        echo "heap_size=N/A"
        echo "procs=N/A"
        echo "go_godebug=$GO_GODEBUG"
        echo "go_gogc=${GO_GOGC:-}"
        echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "go_version=$(go version 2>/dev/null || echo unavailable)"
    } > "$out_dir/run_metadata.env"
    cp -R "$CASE_DIR/go_benchmark" "$out_dir/go_benchmark.snapshot"
    cp "$CASE_DIR/payloads/${PAYLOAD}.json" "$out_dir/payload.snapshot.json"

    local -a child_env=(
        "HOME=$HOME"
        "TERM=${TERM:-xterm}"
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin"
        "CASE_DIR=$CASE_DIR"
        "OUT_DIR=$out_dir"
        "LABEL=$label"
        "CASE27_PAYLOAD=$PAYLOAD"
        "CASE27_CONCURRENCY=$CONC"
        "CASE27_DURATION_MS=$DUR"
        "CASE27_SERVER_PORT=$port"
        "CASE27_CLIENT_TIMEOUT_MS=${CASE27_CLIENT_TIMEOUT_MS:-5000}"
        "CASE27_PAYLOAD_DIR=$CASE_DIR/payloads"
        "CASE27_CLIENT_MODE=$CLIENT_MODE"
        "CASE27_SEND_INTERVAL_MS=$SEND_INTERVAL_MS"
        "CASE27_MAX_INFLIGHT=$MAX_INFLIGHT"
        "GO_GODEBUG=$GO_GODEBUG"
        "GO_GOGC=$GO_GOGC"
    )

    env -i "${child_env[@]}" bash -c '
            set -o pipefail
            export GOCACHE="$OUT_DIR/.gocache"
            export GOTMPDIR="$OUT_DIR/.gotmp"
            mkdir -p "$GOCACHE" "$GOTMPDIR"

            cd "$CASE_DIR/go_benchmark"
            go build -o "$OUT_DIR/benchmark_go" .
            if [ ! -x "$OUT_DIR/benchmark_go" ]; then
                echo "[$LABEL] go build failed"; exit 1
            fi

            cd "$OUT_DIR"
            if [ -n "${GO_GOGC:-}" ]; then
                export GOGC="$GO_GOGC"
            else
                unset GOGC 2>/dev/null || true
            fi

            GODEBUG="$GO_GODEBUG" \
                "$OUT_DIR/benchmark_go" > "$OUT_DIR/stdout.txt" 2> "$OUT_DIR/gc.log" &
            BENCH_PID=$!

            perl -MTime::HiRes=sleep,time -e "
                my \$pid = \$ARGV[0];
                while (kill 0, \$pid) {
                    my \$line = \`ps -o rss=,%cpu= -p \$pid 2>/dev/null\`;
                    chomp \$line; \$line =~ s/^\\s+//;
                    if (length \$line) {
                        my (\$rss, \$cpu) = split /\\s+/, \$line, 2;
                        my \$t = time();
                        my @lt = localtime(int(\$t));
                        my \$frac = sprintf(\"%.3f\", \$t - int(\$t));
                        \$frac =~ s/^0//;
                        printf \"%04d-%02d-%02dT%02d:%02d:%02d%s,%s,%s\\n\",
                            \$lt[5]+1900,\$lt[4]+1,\$lt[3],\$lt[2],\$lt[1],\$lt[0],\$frac,\$rss,\$cpu;
                    }
                    sleep(0.2);
                }
            " "$BENCH_PID" > "$OUT_DIR/rss.csv" 2>/dev/null &
            SAMPLER_PID=$!
            wait "$BENCH_PID"
            ec=$?
            kill "$SAMPLER_PID" 2>/dev/null || true
            wait "$SAMPLER_PID" 2>/dev/null || true
            echo "[$LABEL] bench exit=$ec"
            exit $ec
        '
    local rc=$?
    echo "[INFO] ===== $label done (rc=$rc) ====="
    return $rc
}

# 先跑 CopyGC 基线，再跑 MSGC
run_side "CopyGC" "$COPYGC_DIR" "$COPYGC_SDK" "$COPYGC_STDX" 18226 "" "" "$COPYGC_GC_INTERVAL"
COPYGC_RC=$?

run_side "MSGC" "$MSGC_DIR" "$MSGC_SDK" "$MSGC_STDX" 18240 \
    "$MSGC_HEAP_GROWTH" "$MSGC_SCAVENGE_GROWTH" "$MSGC_GC_INTERVAL"
MSGC_RC=$?

echo "[INFO] CopyGC rc=$COPYGC_RC  MSGC rc=$MSGC_RC"
GO_RC=0
if [[ "$GO_ENABLE" != "0" ]]; then
    run_go_side "Go" "$GO_DIR" 18244
    GO_RC=$?
    echo "[INFO] Go rc=$GO_RC"
fi

compare_args=(
    --run "CopyGC=$COPYGC_DIR"
    --run "MSGC=$MSGC_DIR"
)
if [[ "$GO_ENABLE" != "0" ]]; then
    compare_args+=(--run "Go=$GO_DIR")
fi
MPLCONFIGDIR="$MPLCONFIG_DIR" "$PYTHON_BIN" "$SCRIPT_DIR/compare_results.py" \
    "${compare_args[@]}" \
    --output "$COMPARE_DIR/compare_report.md" \
    || echo "[WARN] compare_results.py failed (non-fatal)"

echo "[DONE] compare_dir=$COMPARE_DIR"
