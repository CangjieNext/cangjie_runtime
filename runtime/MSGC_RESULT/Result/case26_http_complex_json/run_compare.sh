#!/usr/bin/env bash
# ============================================================
# case26 通用对比脚本：MSGC vs CopyGC (+ Go + Java)
#
# SDK 路径可通过环境变量覆盖。其余参数见 README 中的 case26 约定。
#
#   SDK 参数（有默认值，可覆盖）：
#     MSGC_SDK_ROOT       MSGC 的 SDK 根路径（应包含 envsetup.sh）
#     MSGC_STDX_ROOT      MSGC 的平台根目录（脚本内部使用 dynamic/stdx）
#     COPYGC_SDK_ROOT     CopyGC 的 SDK 根路径（应包含 envsetup.sh）
#     COPYGC_STDX_ROOT    CopyGC 的平台根目录（脚本内部使用 dynamic/stdx）
#
#   可选环境变量（有默认值）：
#     CASE26_PAYLOAD       1K|10K|100K|1M        默认 100K
#     CASE26_CONCURRENCY   worker 并发数           默认 1000
#     CASE26_DURATION_MS   压测时长 (ms)           默认 60000
#     HEAP_SIZE            cjHeapSize             默认 16gb
#     PROCS                可选；显式设置时透传 cjProcessorNum
#     MSGC_HEAP_GROWTH     仅传给 MSGC 的 cjHeapGrowth，默认 1
#     MSGC_SCAVENGE_GROWTH 仅传给 MSGC 的 cjScavengeGrowth，默认 1.10
#     MSGC_MIN_GC_THRESHOLD 仅传给 MSGC 的 cjMSMinGCThreshold，默认 20mb；0 表示关闭地板
#     cjMSAdaptiveGrowth    MSGC HeapGrow/Scavenge 自适应开关；默认不设置（静态），1 表示开启
#     CJ_MSGC_BARRIER_COUNT Runtime 入口统计开关；默认 1，0 关闭
#     COPYGC_GC_INTERVAL   仅传给 CopyGC 的 cjGCInterval，默认不设置（如 150ms）
#     MSGC_GC_INTERVAL     仅传给 MSGC 的 cjGCInterval，默认不设置（如 5ms）
#     cjGCBGSweepMaxTime   仅透传给 MSGC 后台 sweep 单 slice 时间预算（如 2ms）
#     CJ_MS_VERIFY_MARK    仅透传给 MSGC runtime 的 verifier 开关，默认不设置
#     CJ_MS_VERIFY_MARK_DETAIL  verifier detail 开关，默认不设置
#     CJ_MS_VERIFY_ROOTS   root 枚举诊断开关，默认不设置
#     CASE26_BODY_DIAG     benchmark 请求/响应体诊断开关，默认不设置
#     CASE26_DUMP_BAD_BODY benchmark 失败请求/响应完整 body 落盘开关，默认不设置
#     CASE26_GRAPH_DIAG    benchmark JSON 图遍历诊断开关，默认不设置
#     CASE26_GRAPH_DIAG_RATE  JSON 图遍历每 N 个节点打印一次，默认不设置
#     CASE26_CLIENT_MODE   sync|async             默认 sync
#     CASE26_SEND_INTERVAL_MS async 发射间隔 (ms)  默认 1
#     CASE26_MAX_INFLIGHT  async 最大在途请求数    默认 32
#     CASE26_PERSISTENT_BYTES 持久堆大小，支持 bytes/mb/gb，默认 5gb
#     CASE26_SAMPLE_DURATION_SECONDS  macOS sample 时长（秒），0 表示关闭，默认 0
#     CASE26_SAMPLE_DELAY_SECONDS     启动后等待多久再采样（秒），默认 8
#     rss.csv 额外列                 Cangjie side 记录累计 user/sys CPU 秒，可用成功数计算每请求 CPU 时间
#     COPYGC_ENABLE       是否执行 CopyGC side     默认 1
#     MSGC_ENABLE         是否执行 MSGC side       默认 1
#     GO_ENABLE            是否加入 Go mirror side 默认 1
#     GO_GODEBUG           Go benchmark 的 GODEBUG 默认 gctrace=1
#     GO_GOGC              可选，透传给 Go side
#     JAVA_ENABLE          是否加入 Java mirror side 默认 1
#     JAVA_HOME_DEFAULT    Java 25 安装根目录，默认 /opt/homebrew/opt/openjdk@25
#     JAVA_BIN             Java 命令，默认 $JAVA_HOME_DEFAULT/bin/java
#     JAVAC_BIN            javac 命令，默认 $JAVA_HOME_DEFAULT/bin/javac
#     JAVA_OPTS            可选 Java 运行参数
#     JAVA_GC_OPTS         可选 Java GC 日志参数；为空时按 JDK 版本自动配置
#     JAVA_BENCHMARK_SOURCE Java 基准源码；默认 java_benchmark/Case27HttpComplexJsonBenchmark.java
#     JAVA_BENCHMARK_TEST_SOURCE 可选 Java 测试源码；设为空时跳过测试编译
#     JAVA_BENCHMARK_MAIN  Java 基准主类；默认 Case27HttpComplexJsonBenchmark
#     JAVA_BENCHMARK_TEST_MAIN 可选 Java 测试主类；设为空时跳过测试运行
#     JAVA_BENCHMARK_CLASSPATH Java 基准依赖 classpath；默认空
#
#   用法示例：
#     MSGC_SDK_ROOT=/path/to/msgc/sdk \
#     MSGC_STDX_ROOT=/path/to/msgc/platform-root \
#     COPYGC_SDK_ROOT=/path/to/copygc/sdk \
#     COPYGC_STDX_ROOT=/path/to/copygc/platform-root \
#     CASE26_PAYLOAD=10K CASE26_CONCURRENCY=8 \
#     ./run_compare.sh
# ============================================================
set -uo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_DIR="$CASE_DIR/script"
PYTHON_BIN="${PYTHON_BIN:-python3}"
SHARED_MPLCONFIG_DIR="$CASE_DIR/result/.mplconfig"
COPYGC_BENCHMARK_SOURCE="${COPYGC_BENCHMARK_SOURCE:-$CASE_DIR/cj_copygc_benchmark/benchmark.cj}"
MSGC_BENCHMARK_SOURCE="${MSGC_BENCHMARK_SOURCE:-$CASE_DIR/cj_msgc_benchmark/benchmark.cj}"
if [[ ! -f "$SCRIPT_DIR/compare_results.py" ]]; then
    echo "[ERROR] local compare script not found: $SCRIPT_DIR/compare_results.py" >&2
    exit 1
fi

# ---------- SDK 路径（可通过环境变量覆盖） ----------
MSGC_SDK_ROOT="${MSGC_SDK_ROOT:-/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc}"
MSGC_STDX_ROOT="${MSGC_STDX_ROOT:-$MSGC_SDK_ROOT/darwin_aarch64_cjnative}"
COPYGC_SDK_ROOT="${COPYGC_SDK_ROOT:-/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie}"
COPYGC_STDX_ROOT="${COPYGC_STDX_ROOT:-/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative}"

MSGC_SDK="$MSGC_SDK_ROOT"
COPYGC_SDK="$COPYGC_SDK_ROOT"

MSGC_STDX="${MSGC_STDX_ROOT}/dynamic/stdx"
COPYGC_STDX="${COPYGC_STDX_ROOT}/dynamic/stdx"

# ---------- 可选参数（与 README 中的 case26 环境变量约定对齐） ----------
PAYLOAD="${CASE26_PAYLOAD:-100K}"
CONC="${CASE26_CONCURRENCY:-1000}"
DUR="${CASE26_DURATION_MS:-60000}"
HEAP_SIZE="${HEAP_SIZE:-16gb}"
PROCS="${PROCS:-}"
MSGC_HEAP_GROWTH="${MSGC_HEAP_GROWTH:-1}"
MSGC_SCAVENGE_GROWTH="${MSGC_SCAVENGE_GROWTH:-1.10}"
MSGC_MIN_GC_THRESHOLD="${MSGC_MIN_GC_THRESHOLD:-20mb}"
COPYGC_GC_INTERVAL="${COPYGC_GC_INTERVAL:-}"
MSGC_GC_INTERVAL="${MSGC_GC_INTERVAL:-}"
CLIENT_MODE="${CASE26_CLIENT_MODE:-sync}"
SEND_INTERVAL_MS="${CASE26_SEND_INTERVAL_MS:-1}"
MAX_INFLIGHT="${CASE26_MAX_INFLIGHT:-32}"
PERSISTENT_BYTES_INPUT="${CASE26_PERSISTENT_BYTES:-5368709120}"
if ! PERSISTENT_BYTES="$(
    bash "$SCRIPT_DIR/normalize_byte_size.sh" "$PERSISTENT_BYTES_INPUT"
)"; then
    exit 1
fi
PERSISTENT_WORKERS="${CASE26_PERSISTENT_WORKERS:-8}"
COPYGC_ENABLE="${COPYGC_ENABLE:-1}"
MSGC_ENABLE="${MSGC_ENABLE:-1}"
GO_ENABLE="${GO_ENABLE:-1}"
GO_GODEBUG="${GO_GODEBUG:-gctrace=1}"
GO_GOGC="${GO_GOGC:-}"
GO_GOMEMLIMIT="${GO_GOMEMLIMIT:-16GiB}"
JAVA_ENABLE="${JAVA_ENABLE:-1}"
JAVA_HOME_DEFAULT="${JAVA_HOME_DEFAULT:-/opt/homebrew/opt/openjdk@25}"
JAVA_BIN="${JAVA_BIN:-$JAVA_HOME_DEFAULT/bin/java}"
JAVAC_BIN="${JAVAC_BIN:-$JAVA_HOME_DEFAULT/bin/javac}"
# HotSpot accepts g/m/k suffixes, but not the cjHeapSize-style gb/mb suffixes.
# Keep an explicitly supplied JAVA_OPTS unchanged; otherwise cap Java with HEAP_SIZE.
JAVA_HEAP_SIZE=$(printf '%s' "$HEAP_SIZE" | tr '[:upper:]' '[:lower:]')
JAVA_HEAP_SIZE="${JAVA_HEAP_SIZE%b}"
JAVA_OPTS="${JAVA_OPTS:--Xmx${JAVA_HEAP_SIZE}}"
JAVA_GC_OPTS="${JAVA_GC_OPTS:-}"
JAVA_BENCHMARK_SOURCE="${JAVA_BENCHMARK_SOURCE-$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmark.java}"
JAVA_BENCHMARK_TEST_SOURCE="${JAVA_BENCHMARK_TEST_SOURCE-$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmarkTest.java}"
JAVA_BENCHMARK_MAIN="${JAVA_BENCHMARK_MAIN-Case27HttpComplexJsonBenchmark}"
JAVA_BENCHMARK_TEST_MAIN="${JAVA_BENCHMARK_TEST_MAIN-Case27HttpComplexJsonBenchmarkTest}"
JAVA_BENCHMARK_CLASSPATH="${JAVA_BENCHMARK_CLASSPATH-}"
CASE26_SAMPLE_DURATION_SECONDS="${CASE26_SAMPLE_DURATION_SECONDS:-0}"
CASE26_SAMPLE_DELAY_SECONDS="${CASE26_SAMPLE_DELAY_SECONDS:-8}"

for enable_name in COPYGC_ENABLE MSGC_ENABLE GO_ENABLE JAVA_ENABLE; do
    enable_value="${!enable_name}"
    if [[ "$enable_value" != "0" && "$enable_value" != "1" ]]; then
        echo "[ERROR] $enable_name must be 0 or 1, got '$enable_value'" >&2
        exit 1
    fi
done
ENABLED_SIDE_COUNT=$((COPYGC_ENABLE + MSGC_ENABLE + GO_ENABLE + JAVA_ENABLE))
if [[ "$ENABLED_SIDE_COUNT" -eq 0 ]]; then
    echo "[ERROR] at least one of COPYGC_ENABLE, MSGC_ENABLE, GO_ENABLE, or JAVA_ENABLE must be 1" >&2
    exit 1
fi

if [[ ! -f "$CASE_DIR/payloads/${PAYLOAD}.json" ]]; then
    echo "[ERROR] payloads/${PAYLOAD}.json not found" >&2
    exit 1
fi
if [[ "$COPYGC_ENABLE" != "0" && ! -f "$COPYGC_BENCHMARK_SOURCE" ]]; then
    echo "[ERROR] CopyGC benchmark source not found: $COPYGC_BENCHMARK_SOURCE" >&2
    exit 1
fi
if [[ "$MSGC_ENABLE" != "0" && ! -f "$MSGC_BENCHMARK_SOURCE" ]]; then
    echo "[ERROR] MSGC benchmark source not found: $MSGC_BENCHMARK_SOURCE" >&2
    exit 1
fi
if [[ "$JAVA_ENABLE" != "0" ]]; then
    if ! command -v "$JAVA_BIN" >/dev/null 2>&1; then
        echo "[ERROR] Java enabled but JAVA_BIN is unavailable: $JAVA_BIN" >&2
        exit 1
    fi
    if ! command -v "$JAVAC_BIN" >/dev/null 2>&1; then
        echo "[ERROR] Java enabled but JAVAC_BIN is unavailable: $JAVAC_BIN" >&2
        exit 1
    fi
    JAVA_BIN="$(command -v "$JAVA_BIN")"
    JAVAC_BIN="$(command -v "$JAVAC_BIN")"
    if [[ ! -f "$JAVA_BENCHMARK_SOURCE" ]]; then
        echo "[ERROR] Java benchmark source not found: $JAVA_BENCHMARK_SOURCE" >&2
        exit 1
    fi
    if [[ -n "$JAVA_BENCHMARK_TEST_SOURCE" && ! -f "$JAVA_BENCHMARK_TEST_SOURCE" ]]; then
        echo "[ERROR] Java benchmark test source not found: $JAVA_BENCHMARK_TEST_SOURCE" >&2
        exit 1
    fi
    if [[ -n "$JAVA_BENCHMARK_TEST_SOURCE" && -z "$JAVA_BENCHMARK_TEST_MAIN" ]] ||
       [[ -z "$JAVA_BENCHMARK_TEST_SOURCE" && -n "$JAVA_BENCHMARK_TEST_MAIN" ]]; then
        echo "[ERROR] JAVA_BENCHMARK_TEST_SOURCE and JAVA_BENCHMARK_TEST_MAIN must be set together" >&2
        exit 1
    fi
fi

TS="$(date +%Y%m%d)"
PERSISTENT_BYTES_MIB=$((PERSISTENT_BYTES / 1024 / 1024))
PAYLOAD_TAG=$(printf '%s' "$PAYLOAD" | tr '[:upper:]' '[:lower:]')
COMPARE_DIR_BASE="$CASE_DIR/result/${TS}_${PERSISTENT_BYTES_MIB}M_c${CONC}_${PAYLOAD_TAG}_HeapGrow${MSGC_HEAP_GROWTH}_Scavenge${MSGC_SCAVENGE_GROWTH}_compare"
COMPARE_DIR="$COMPARE_DIR_BASE"
COMPARE_DIR_SUFFIX=1
while [[ -e "$COMPARE_DIR" ]]; do
    COMPARE_DIR="${COMPARE_DIR_BASE}_${COMPARE_DIR_SUFFIX}"
    COMPARE_DIR_SUFFIX=$((COMPARE_DIR_SUFFIX + 1))
done
COPYGC_DIR="$COMPARE_DIR/copygc"
MSGC_DIR="$COMPARE_DIR/msgc"
GO_DIR="$COMPARE_DIR/go"
JAVA_DIR="$COMPARE_DIR/java"
MPLCONFIG_DIR="$SHARED_MPLCONFIG_DIR"
if [[ "$COPYGC_ENABLE" != "0" ]]; then
    mkdir -p "$COPYGC_DIR"
fi
if [[ "$MSGC_ENABLE" != "0" ]]; then
    mkdir -p "$MSGC_DIR"
fi
if [[ "$GO_ENABLE" != "0" ]]; then
    mkdir -p "$GO_DIR"
fi
if [[ "$JAVA_ENABLE" != "0" ]]; then
    mkdir -p "$JAVA_DIR"
fi
mkdir -p "$MPLCONFIG_DIR"
echo "[INFO] compare dir: $COMPARE_DIR"
echo "[INFO] config: payload=$PAYLOAD concurrency=$CONC duration=${DUR}ms heap=$HEAP_SIZE persistent=${PERSISTENT_BYTES_INPUT}(${PERSISTENT_BYTES}B) proc=${PROCS:-auto} mode=$CLIENT_MODE msgcMinGCThreshold=$MSGC_MIN_GC_THRESHOLD"
echo "[INFO] enabled: copygc=$COPYGC_ENABLE msgc=$MSGC_ENABLE go=$GO_ENABLE java=$JAVA_ENABLE"

run_side() {
    local label="$1"
    local out_dir="$2"
    local sdk_root="$3"
    local stdx_dir="$4"
    local port="$5"
    local runtime_heap_growth="${6:-}"
    local runtime_scavenge_growth="${7:-}"
    local runtime_gc_interval="${8:-}"
    local runtime_min_gc_threshold="${9:-}"
    local cjc_gc_mode="${10}"
    local benchmark_source="${11}"
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
        echo "case=case26_http_complex_json"
        echo "label=$label"
        echo "sdk_root=$sdk_root"
        echo "stdx_dir=$stdx_dir"
        echo "cjc_gc_mode=$cjc_gc_mode"
        echo "benchmark_source=$benchmark_source"
        echo "payload=$PAYLOAD"
        echo "concurrency=$CONC"
        echo "duration_ms=$DUR"
        echo "persistent_bytes=$PERSISTENT_BYTES"
        echo "persistent_workers=$PERSISTENT_WORKERS"
        echo "client_timeout_ms=${CASE26_CLIENT_TIMEOUT_MS:-5000}"
        echo "client_mode=$CLIENT_MODE"
        echo "send_interval_ms=$SEND_INTERVAL_MS"
        echo "max_inflight=$MAX_INFLIGHT"
        echo "server_port=$port"
        echo "original_port=$orig_port"
        echo "heap_size=$HEAP_SIZE"
        echo "cj_heap_growth=${runtime_heap_growth:-}"
        echo "cj_scavenge_growth=${runtime_scavenge_growth:-}"
        echo "cj_ms_adaptive_growth=${cjMSAdaptiveGrowth:-}"
        echo "cj_msgc_barrier_count=${CJ_MSGC_BARRIER_COUNT:-1}"
        echo "cj_gc_interval=${runtime_gc_interval:-}"
        echo "cj_ms_min_gc_threshold=${runtime_min_gc_threshold:-}"
        echo "procs=${PROCS:-}"
        echo "schdfd_timeout_diag=${CJ_SCHDFD_TIMEOUT_DIAG:-0}"
        echo "cj_ms_disable_assist=${CJ_MS_DISABLE_ASSIST:-}"
        echo "cj_ms_bg_scavenge=${CJ_MS_BG_SCAVENGE:-}"
        echo "cj_ms_pin_hot_free=${CJ_MS_PIN_HOT_FREE:-}"
        echo "cj_ms_verify_mark=${CJ_MS_VERIFY_MARK:-}"
        echo "cj_ms_verify_mark_detail=${CJ_MS_VERIFY_MARK_DETAIL:-}"
        echo "cj_ms_verify_roots=${CJ_MS_VERIFY_ROOTS:-}"
        echo "case26_body_diag=${CASE26_BODY_DIAG:-}"
        echo "case26_dump_bad_body=${CASE26_DUMP_BAD_BODY:-}"
        echo "case26_dump_dir=$out_dir"
        echo "case26_graph_diag=${CASE26_GRAPH_DIAG:-}"
        echo "case26_graph_diag_rate=${CASE26_GRAPH_DIAG_RATE:-}"
        echo "sample_duration_seconds=$CASE26_SAMPLE_DURATION_SECONDS"
        echo "sample_delay_seconds=$CASE26_SAMPLE_DELAY_SECONDS"
        echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "runtime_dylib_md5=$(md5 -q "$sdk_root"/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib 2>/dev/null || echo 'N/A')"
    } > "$out_dir/run_metadata.env"
    cp "$benchmark_source" "$out_dir/benchmark.cj.snapshot"
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
        "RUNTIME_MIN_GC_THRESHOLD=$runtime_min_gc_threshold"
        "CJC_GC_MODE=$cjc_gc_mode"
        "BENCH_SRC=$benchmark_source"
        "PAYLOAD_DIR=$CASE_DIR/payloads"
        "CASE26_PAYLOAD=$PAYLOAD"
        "CASE26_CONCURRENCY=$CONC"
        "CASE26_DURATION_MS=$DUR"
        "CASE26_PERSISTENT_BYTES=$PERSISTENT_BYTES"
        "CASE26_PERSISTENT_WORKERS=$PERSISTENT_WORKERS"
        "CASE26_SERVER_PORT=$port"
        "CASE26_CLIENT_TIMEOUT_MS=${CASE26_CLIENT_TIMEOUT_MS:-5000}"
        "CASE26_PAYLOAD_DIR=$CASE_DIR/payloads"
        "CASE26_DUMP_DIR=$out_dir"
        "CASE26_CLIENT_MODE=$CLIENT_MODE"
        "CASE26_SEND_INTERVAL_MS=$SEND_INTERVAL_MS"
        "CASE26_MAX_INFLIGHT=$MAX_INFLIGHT"
        "SAMPLE_DURATION_SECONDS=$CASE26_SAMPLE_DURATION_SECONDS"
        "SAMPLE_DELAY_SECONDS=$CASE26_SAMPLE_DELAY_SECONDS"
    )
    local optional_var=""
    local optional_value=""
    for optional_var in \
        CASE26_BODY_DIAG \
        CASE26_DUMP_BAD_BODY \
        CASE26_GRAPH_DIAG \
        CASE26_GRAPH_DIAG_RATE \
        CJ_SCHDFD_TIMEOUT_DIAG \
        CJ_MS_DISABLE_ASSIST \
        CJ_MS_ASSIST_RATIO_SCALE \
        CJ_MS_ASSIST_DEBT_TRIGGER \
        CJ_MS_BG_SCAVENGE \
        CJ_MS_PIN_HOT_FREE \
        CJ_MS_VERIFY_MARK \
        CJ_MS_VERIFY_MARK_DETAIL \
        CJ_MS_VERIFY_ROOTS \
        CJ_MSGC_BARRIER_COUNT \
        cjMSAdaptiveGrowth \
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
            echo "[$LABEL] cjc gc mode=$CJC_GC_MODE"
            echo "[$LABEL] CJ_MS_DISABLE_ASSIST=${CJ_MS_DISABLE_ASSIST:-<default>}"
            echo "[$LABEL] CJ_MS_VERIFY_MARK=${CJ_MS_VERIFY_MARK:-<default>}"
            echo "[$LABEL] CJ_MS_VERIFY_MARK_DETAIL=${CJ_MS_VERIFY_MARK_DETAIL:-<default>}"
            echo "[$LABEL] CJ_MS_VERIFY_ROOTS=${CJ_MS_VERIFY_ROOTS:-<default>}"
            echo "[$LABEL] cjGCBGSweepMaxTime=${cjGCBGSweepMaxTime:-<default>}"
            echo "[$LABEL] cjScavengeGrowth=${RUNTIME_SCAVENGE_GROWTH:-<default>}"
            echo "[$LABEL] cjMSAdaptiveGrowth=${cjMSAdaptiveGrowth:-0}"
            echo "[$LABEL] CJ_MSGC_BARRIER_COUNT=${CJ_MSGC_BARRIER_COUNT:-1}"
            echo "[$LABEL] cjMSMinGCThreshold=${RUNTIME_MIN_GC_THRESHOLD:-<default>}"
            echo "[$LABEL] CASE26_BODY_DIAG=${CASE26_BODY_DIAG:-<default>}"
            echo "[$LABEL] CASE26_DUMP_BAD_BODY=${CASE26_DUMP_BAD_BODY:-<default>}"
            echo "[$LABEL] CASE26_DUMP_DIR=${CASE26_DUMP_DIR:-<default>}"
            echo "[$LABEL] CASE26_GRAPH_DIAG=${CASE26_GRAPH_DIAG:-<default>}"
            echo "[$LABEL] CASE26_GRAPH_DIAG_RATE=${CASE26_GRAPH_DIAG_RATE:-<default>}"

            cd "$OUT_DIR"
            cjc_args=("-O2" "$BENCH_SRC")
            if [ -n "$CJC_GC_MODE" ]; then
                cjc_args+=("--gc-mode=$CJC_GC_MODE")
            fi
            cjc "${cjc_args[@]}" \
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

            if [ -n "${RUNTIME_MIN_GC_THRESHOLD:-}" ]; then
                echo "[$LABEL] cjMSMinGCThreshold=$RUNTIME_MIN_GC_THRESHOLD"
                export cjMSMinGCThreshold="$RUNTIME_MIN_GC_THRESHOLD"
            else
                echo "[$LABEL] cjMSMinGCThreshold=<default>"
                unset cjMSMinGCThreshold 2>/dev/null || true
            fi

            if [ -n "${PROCS:-}" ]; then
                export cjProcessorNum="$PROCS"
            else
                unset cjProcessorNum 2>/dev/null || true
            fi

            # cjHeapSize is the MSGC soft memory limit.  Leave the virtual heap
            # reservation unset so the runtime uses the host-memory default.
            echo "[$LABEL] cjHeapSize=$HEAP_SIZE (soft limit; virtual heap=runtime default)"
            MRT_REPORT="$OUT_DIR/gc.log" \
            MRT_LOG_FILE_SIZE="200mb" \
            cjHeapSize="$HEAP_SIZE" \
                "$OUT_DIR/benchmark" > "$OUT_DIR/stdout.txt" 2>&1 &
            BENCH_PID=$!

            SAMPLE_PID=""
            if [[ "$SAMPLE_DURATION_SECONDS" =~ ^[1-9][0-9]*$ ]] && [[ -x /usr/bin/sample ]]; then
                (
                    sleep "$SAMPLE_DELAY_SECONDS"
                    if kill -0 "$BENCH_PID" 2>/dev/null; then
                        /usr/bin/sample "$BENCH_PID" "$SAMPLE_DURATION_SECONDS" 1 \
                            -file "$OUT_DIR/sample_${SAMPLE_DURATION_SECONDS}s.txt" \
                            > "$OUT_DIR/sample.stdout.txt" 2> "$OUT_DIR/sample.stderr.txt"
                    fi
                ) &
                SAMPLE_PID=$!
            fi

            perl -MTime::HiRes=sleep,time -e "
                my \$pid = \$ARGV[0];
                while (kill 0, \$pid) {
                    my \$line = \`ps -o rss=,%cpu=,utime=,stime= -p \$pid 2>/dev/null\`;
                    chomp \$line; \$line =~ s/^\s+//;
                    if (length \$line) {
                        my (\$rss, \$cpu, \$utime, \$stime) = split /\s+/, \$line, 4;
                        my \$to_seconds = sub {
                            my \$total = 0;
                            for my \$part (split /:/, \$_[0]) {
                                \$total = \$total * 60 + \$part;
                            }
                            return \$total;
                        };
                        my \$cpu_seconds = \$to_seconds->(\$utime) + \$to_seconds->(\$stime);
                        my \$t = time();
                        my @lt = localtime(int(\$t));
                        my \$frac = sprintf(\"%.3f\", \$t - int(\$t));
                        \$frac =~ s/^0//;
                        printf \"%04d-%02d-%02dT%02d:%02d:%02d%s,%s,%s,%s,%s,%.2f\n\",
                            \$lt[5]+1900,\$lt[4]+1,\$lt[3],\$lt[2],\$lt[1],\$lt[0],\$frac,\$rss,\$cpu,\$utime,\$stime,\$cpu_seconds;
                    }
                    sleep(0.2);
                }
            " "$BENCH_PID" > "$OUT_DIR/rss.csv" 2>/dev/null &
            SAMPLER_PID=$!
            wait "$BENCH_PID"
            ec=$?
            kill "$SAMPLER_PID" 2>/dev/null || true
            wait "$SAMPLER_PID" 2>/dev/null || true
            if [[ -n "$SAMPLE_PID" ]]; then
                wait "$SAMPLE_PID" || true
            fi
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
        echo "case=case26_http_complex_json"
        echo "label=$label"
        echo "payload=$PAYLOAD"
        echo "concurrency=$CONC"
        echo "duration_ms=$DUR"
        echo "persistent_bytes=$PERSISTENT_BYTES"
        echo "persistent_workers=$PERSISTENT_WORKERS"
        echo "client_timeout_ms=${CASE26_CLIENT_TIMEOUT_MS:-5000}"
        echo "client_mode=$CLIENT_MODE"
        echo "send_interval_ms=$SEND_INTERVAL_MS"
        echo "max_inflight=$MAX_INFLIGHT"
        echo "server_port=$port"
        echo "original_port=$orig_port"
        echo "heap_size=N/A"
        echo "procs=N/A"
        echo "go_godebug=$GO_GODEBUG"
        echo "go_gogc=${GO_GOGC:-}"
        echo "go_gomemlimit=$GO_GOMEMLIMIT"
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
        "CASE26_PAYLOAD=$PAYLOAD"
        "CASE26_CONCURRENCY=$CONC"
        "CASE26_DURATION_MS=$DUR"
        "CASE26_PERSISTENT_BYTES=$PERSISTENT_BYTES"
        "CASE26_PERSISTENT_WORKERS=$PERSISTENT_WORKERS"
        "CASE26_SERVER_PORT=$port"
        "CASE26_CLIENT_TIMEOUT_MS=${CASE26_CLIENT_TIMEOUT_MS:-5000}"
        "CASE26_PAYLOAD_DIR=$CASE_DIR/payloads"
        "CASE26_CLIENT_MODE=$CLIENT_MODE"
        "CASE26_SEND_INTERVAL_MS=$SEND_INTERVAL_MS"
        "CASE26_MAX_INFLIGHT=$MAX_INFLIGHT"
        "GO_GODEBUG=$GO_GODEBUG"
        "GO_GOGC=$GO_GOGC"
        "GOMEMLIMIT=$GO_GOMEMLIMIT"
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

run_java_side() {
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

    echo "[INFO] ===== $label (Java mirror benchmark) ====="
    {
        echo "case=case26_http_complex_json"
        echo "label=$label"
        echo "payload=$PAYLOAD"
        echo "concurrency=$CONC"
        echo "duration_ms=$DUR"
        echo "persistent_bytes=$PERSISTENT_BYTES"
        echo "persistent_workers=$PERSISTENT_WORKERS"
        echo "client_timeout_ms=${CASE26_CLIENT_TIMEOUT_MS:-5000}"
        echo "client_mode=$CLIENT_MODE"
        echo "send_interval_ms=$SEND_INTERVAL_MS"
        echo "max_inflight=$MAX_INFLIGHT"
        echo "server_port=$port"
        echo "original_port=$orig_port"
        echo "heap_size=N/A"
        echo "procs=N/A"
        echo "java_bin=$JAVA_BIN"
        echo "javac_bin=$JAVAC_BIN"
        echo "java_opts=$JAVA_OPTS"
        echo "java_gc_opts=$JAVA_GC_OPTS"
        echo "java_benchmark_source=$JAVA_BENCHMARK_SOURCE"
        echo "java_benchmark_test_source=$JAVA_BENCHMARK_TEST_SOURCE"
        echo "java_benchmark_main=$JAVA_BENCHMARK_MAIN"
        echo "java_benchmark_test_main=$JAVA_BENCHMARK_TEST_MAIN"
        echo "java_benchmark_classpath=$JAVA_BENCHMARK_CLASSPATH"
        echo "java_version=$("$JAVA_BIN" -version 2>&1 | sed -n '1p')"
        echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$out_dir/run_metadata.env"
    cp "$JAVA_BENCHMARK_SOURCE" \
        "$out_dir/$(basename "$JAVA_BENCHMARK_SOURCE").snapshot"
    if [[ -n "$JAVA_BENCHMARK_TEST_SOURCE" ]]; then
        cp "$JAVA_BENCHMARK_TEST_SOURCE" \
            "$out_dir/$(basename "$JAVA_BENCHMARK_TEST_SOURCE").snapshot"
    fi
    cp "$CASE_DIR/payloads/${PAYLOAD}.json" "$out_dir/payload.snapshot.json"

    local -a child_env=(
        "HOME=$HOME"
        "TERM=${TERM:-xterm}"
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin"
        "CASE_DIR=$CASE_DIR"
        "OUT_DIR=$out_dir"
        "LABEL=$label"
        "JAVA_BIN=$JAVA_BIN"
        "JAVAC_BIN=$JAVAC_BIN"
        "JAVA_OPTS=$JAVA_OPTS"
        "JAVA_GC_OPTS=$JAVA_GC_OPTS"
        "JAVA_BENCHMARK_SOURCE=$JAVA_BENCHMARK_SOURCE"
        "JAVA_BENCHMARK_TEST_SOURCE=$JAVA_BENCHMARK_TEST_SOURCE"
        "JAVA_BENCHMARK_MAIN=$JAVA_BENCHMARK_MAIN"
        "JAVA_BENCHMARK_TEST_MAIN=$JAVA_BENCHMARK_TEST_MAIN"
        "JAVA_BENCHMARK_CLASSPATH=$JAVA_BENCHMARK_CLASSPATH"
        "CASE26_PAYLOAD=$PAYLOAD"
        "CASE26_CONCURRENCY=$CONC"
        "CASE26_DURATION_MS=$DUR"
        "CASE26_PERSISTENT_BYTES=$PERSISTENT_BYTES"
        "CASE26_PERSISTENT_WORKERS=$PERSISTENT_WORKERS"
        "CASE26_SERVER_PORT=$port"
        "CASE26_CLIENT_TIMEOUT_MS=${CASE26_CLIENT_TIMEOUT_MS:-5000}"
        "CASE26_PAYLOAD_DIR=$CASE_DIR/payloads"
        "CASE26_CLIENT_MODE=$CLIENT_MODE"
        "CASE26_SEND_INTERVAL_MS=$SEND_INTERVAL_MS"
        "CASE26_MAX_INFLIGHT=$MAX_INFLIGHT"
        "SAMPLE_DURATION_SECONDS=$CASE26_SAMPLE_DURATION_SECONDS"
        "SAMPLE_DELAY_SECONDS=$CASE26_SAMPLE_DELAY_SECONDS"
    )

    env -i "${child_env[@]}" bash -c '
            set -o pipefail
            mkdir -p "$OUT_DIR/classes"
            echo 125 > "$OUT_DIR/compile.exitcode"
            echo 125 > "$OUT_DIR/test.exitcode"
            echo 125 > "$OUT_DIR/run.exitcode"

            version_line="$("$JAVA_BIN" -version 2>&1 | sed -n "1p")"
            version_token="$(printf "%s\n" "$version_line" | sed -E "s/.*version \"([^\"]+)\".*/\1/")"
            if [[ "$version_token" == 1.* ]]; then
                java_major="$(printf "%s\n" "$version_token" | cut -d. -f2)"
            else
                java_major="${version_token%%.*}"
            fi
            if [[ ! "$java_major" =~ ^[0-9]+$ ]]; then
                echo "[$LABEL] could not detect Java major version from: $version_line" >&2
                echo 1 > "$OUT_DIR/compile.exitcode"
                exit 1
            fi

            module_opts=()
            if ((java_major >= 9)); then
                module_opts=(--add-modules jdk.httpserver)
            fi
            java_opts=()
            if [[ -n "$JAVA_OPTS" ]]; then
                read -r -a java_opts <<< "$JAVA_OPTS"
            fi
            gc_opts=()
            if [[ -n "$JAVA_GC_OPTS" ]]; then
                read -r -a gc_opts <<< "$JAVA_GC_OPTS"
            elif ((java_major >= 9)); then
                gc_opts=("-Xlog:gc*:file=$OUT_DIR/gc.log:time,uptime,level,tags")
            else
                gc_opts=(
                    "-Xloggc:$OUT_DIR/gc.log"
                    -XX:+PrintGCDetails
                    -XX:+PrintGCDateStamps
                    -XX:+PrintGCTimeStamps
                )
            fi
            {
                echo "java_major=$java_major"
                printf "module_opts="; printf "%q " "${module_opts[@]}"; echo
                printf "java_opts="; printf "%q " "${java_opts[@]}"; echo
                printf "java_gc_opts="; printf "%q " "${gc_opts[@]}"; echo
                echo "java_benchmark_source=$JAVA_BENCHMARK_SOURCE"
                echo "java_benchmark_test_source=$JAVA_BENCHMARK_TEST_SOURCE"
                echo "java_benchmark_main=$JAVA_BENCHMARK_MAIN"
                echo "java_benchmark_test_main=$JAVA_BENCHMARK_TEST_MAIN"
                echo "java_benchmark_classpath=$JAVA_BENCHMARK_CLASSPATH"
            } > "$OUT_DIR/java_command.env"

            compile_sources=("$JAVA_BENCHMARK_SOURCE")
            if [[ -n "$JAVA_BENCHMARK_TEST_SOURCE" ]]; then
                compile_sources+=("$JAVA_BENCHMARK_TEST_SOURCE")
            fi
            compile_classpath=()
            if [[ -n "$JAVA_BENCHMARK_CLASSPATH" ]]; then
                compile_classpath=(-cp "$JAVA_BENCHMARK_CLASSPATH")
            fi
            "$JAVAC_BIN" "${module_opts[@]}" "${compile_classpath[@]}" \
                -d "$OUT_DIR/classes" "${compile_sources[@]}" \
                > "$OUT_DIR/compile.stdout.txt" 2> "$OUT_DIR/compile.stderr.txt"
            compile_rc=$?
            echo "$compile_rc" > "$OUT_DIR/compile.exitcode"
            if ((compile_rc != 0)); then
                echo "[$LABEL] javac failed (rc=$compile_rc)" >&2
                exit "$compile_rc"
            fi

            runtime_classpath="$OUT_DIR/classes"
            if [[ -n "$JAVA_BENCHMARK_CLASSPATH" ]]; then
                runtime_classpath="$runtime_classpath:$JAVA_BENCHMARK_CLASSPATH"
            fi
            test_rc=0
            : > "$OUT_DIR/test.stdout.txt"
            : > "$OUT_DIR/test.stderr.txt"
            if [[ -n "$JAVA_BENCHMARK_TEST_SOURCE" && -n "$JAVA_BENCHMARK_TEST_MAIN" ]]; then
                "$JAVA_BIN" "${module_opts[@]}" "${java_opts[@]}" \
                    -cp "$runtime_classpath" "$JAVA_BENCHMARK_TEST_MAIN" \
                    > "$OUT_DIR/test.stdout.txt" 2> "$OUT_DIR/test.stderr.txt"
                test_rc=$?
            fi
            echo "$test_rc" > "$OUT_DIR/test.exitcode"
            if ((test_rc != 0)); then
                echo "[$LABEL] Java benchmark test failed (rc=$test_rc)" >&2
                exit "$test_rc"
            fi

            : > "$OUT_DIR/gc.log"
            "$JAVA_BIN" "${module_opts[@]}" "${java_opts[@]}" "${gc_opts[@]}" \
                -cp "$runtime_classpath" "$JAVA_BENCHMARK_MAIN" \
                > "$OUT_DIR/stdout.txt" 2> "$OUT_DIR/stderr.txt" &
            BENCH_PID=$!

            SAMPLE_PID=""
            if [[ "$SAMPLE_DURATION_SECONDS" =~ ^[1-9][0-9]*$ ]] && [[ -x /usr/bin/sample ]]; then
                (
                    sleep "$SAMPLE_DELAY_SECONDS"
                    if kill -0 "$BENCH_PID" 2>/dev/null; then
                        /usr/bin/sample "$BENCH_PID" "$SAMPLE_DURATION_SECONDS" 1 \
                            -file "$OUT_DIR/sample_${SAMPLE_DURATION_SECONDS}s.txt" \
                            > "$OUT_DIR/sample.stdout.txt" 2> "$OUT_DIR/sample.stderr.txt"
                    fi
                ) &
                SAMPLE_PID=$!
            fi

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
            run_rc=$?
            kill "$SAMPLER_PID" 2>/dev/null || true
            wait "$SAMPLER_PID" 2>/dev/null || true
            if [[ -n "$SAMPLE_PID" ]]; then
                wait "$SAMPLE_PID" || true
            fi
            if ((run_rc == 0)) && ! grep -q "^\[RESULT\] total=" "$OUT_DIR/stdout.txt"; then
                echo "[$LABEL] Java benchmark exited without a result line" >&2
                run_rc=1
            fi
            echo "$run_rc" > "$OUT_DIR/run.exitcode"
            echo "[$LABEL] bench exit=$run_rc"
            exit "$run_rc"
        '
    local rc=$?
    echo "[INFO] ===== $label done (rc=$rc) ====="
    return $rc
}

COPYGC_RC=0
if [[ "$COPYGC_ENABLE" != "0" ]]; then
    run_side "CopyGC" "$COPYGC_DIR" "$COPYGC_SDK" "$COPYGC_STDX" 18226 "" "" "$COPYGC_GC_INTERVAL" "" "" "$COPYGC_BENCHMARK_SOURCE"
    COPYGC_RC=$?
    echo "[INFO] CopyGC rc=$COPYGC_RC"
else
    echo "[INFO] ===== CopyGC skipped (COPYGC_ENABLE=0) ====="
fi

MSGC_RC=0
if [[ "$MSGC_ENABLE" != "0" ]]; then
    run_side "MSGC" "$MSGC_DIR" "$MSGC_SDK" "$MSGC_STDX" 18230 \
        "$MSGC_HEAP_GROWTH" "$MSGC_SCAVENGE_GROWTH" "$MSGC_GC_INTERVAL" "$MSGC_MIN_GC_THRESHOLD" "marksweep" "$MSGC_BENCHMARK_SOURCE"
    MSGC_RC=$?
    echo "[INFO] MSGC rc=$MSGC_RC"
else
    echo "[INFO] ===== MSGC skipped (MSGC_ENABLE=0) ====="
fi

GO_RC=0
if [[ "$GO_ENABLE" != "0" ]]; then
    run_go_side "Go" "$GO_DIR" 18234
    GO_RC=$?
    echo "[INFO] Go rc=$GO_RC"
else
    echo "[INFO] ===== Go skipped (GO_ENABLE=0) ====="
fi
JAVA_RC=0
if [[ "$JAVA_ENABLE" != "0" ]]; then
    run_java_side "Java" "$JAVA_DIR" 18238
    JAVA_RC=$?
    echo "[INFO] Java rc=$JAVA_RC"
else
    echo "[INFO] ===== Java skipped (JAVA_ENABLE=0) ====="
fi

compare_args=()
if [[ "$COPYGC_ENABLE" != "0" ]]; then
    compare_args+=(--run "CopyGC=$COPYGC_DIR")
fi
if [[ "$MSGC_ENABLE" != "0" ]]; then
    compare_args+=(--run "MSGC=$MSGC_DIR")
fi
if [[ "$GO_ENABLE" != "0" ]]; then
    compare_args+=(--run "Go=$GO_DIR")
fi
if [[ "$JAVA_ENABLE" != "0" ]]; then
    compare_args+=(--run "Java=$JAVA_DIR")
fi
if [[ "$ENABLED_SIDE_COUNT" -ge 2 ]]; then
    MPLCONFIGDIR="$MPLCONFIG_DIR" "$PYTHON_BIN" "$SCRIPT_DIR/compare_results.py" \
        "${compare_args[@]}" \
        --output "$COMPARE_DIR/compare_report.md" \
        || echo "[WARN] compare_results.py failed (non-fatal)"
else
    echo "[INFO] compare report skipped: at least two enabled sides are required"
fi

echo "[DONE] compare_dir=$COMPARE_DIR"
for side_rc in "$COPYGC_RC" "$MSGC_RC" "$GO_RC" "$JAVA_RC"; do
    if [[ "$side_rc" -ne 0 ]]; then
        exit "$side_rc"
    fi
done
