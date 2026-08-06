#!/usr/bin/env bash
# ============================================================
# case27 通用对比脚本（Linux 版）：MSGC vs CopyGC (+ Go + Java)
#
# 这是 run_compare.sh 的 Linux 移植版。与 macOS 版的差异：
#   - 默认 SDK 路径改为 Linux 路径
#   - 库路径环境变量使用 LD_LIBRARY_PATH（而非 DYLD_LIBRARY_PATH）
#   - 平台目录名为 linux_<arch>_cjnative（自动检测 uname -m）
#   - runtime 动态库为 libcangjie-runtime.so（而非 .dylib）
#   - 链接选项使用 libstdx.*.so（而非 .dylib）
#   - dylib md5 使用 md5sum（而非 md5 -q）
#   - PATH 不含 /opt/homebrew/bin（macOS 专属）
#
# SDK 路径可通过环境变量覆盖。其余参数见 README 中的 case27 约定。
#
#   SDK 参数（有默认值，可覆盖）：
#     MSGC_SDK_ROOT       MSGC 的 SDK 根路径（应包含 envsetup.sh）
#     MSGC_STDX_ROOT      MSGC 的平台根目录（脚本内部使用 dynamic/stdx）
#     COPYGC_SDK_ROOT     CopyGC 的 SDK 根路径（应包含 envsetup.sh）
#     COPYGC_STDX_ROOT    CopyGC 的平台根目录（脚本内部使用 dynamic/stdx）
#     CJ_ARCH             目标架构（aarch64|x86_64），默认取 uname -m
#
#   可选环境变量（有默认值）：
#     CASE27_PAYLOAD       1K|10K|100K|1M        默认 100K
#     CASE27_CONCURRENCY   worker 并发数           默认 1000
#     CASE27_DURATION_MS   压测时长 (ms)           默认 60000
#     CASE27_NOFILE_LIMIT  进程 fd soft limit       默认 65535
#     HEAP_SIZE            cjHeapSize             默认 16gb
#     PROCS                可选；透传 cjProcessorNum，并设置 Go 的 GOMAXPROCS
#     MSGC_HEAP_GROWTH     仅传给 MSGC 的 cjHeapGrowth，默认 1
#     MSGC_SCAVENGE_GROWTH 仅传给 MSGC 的 cjScavengeGrowth，默认 5
#     MSGC_MIN_GC_THRESHOLD 仅传给 MSGC 的 cjMSMinGCThreshold，默认 20mb；0 表示关闭地板
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
#     JAVA_ENABLE          是否加入 Java mirror side 默认 1
#     JAVA_BIN             Java 运行命令，默认本机 Java 25
#     JAVAC_BIN            Java 编译命令，默认本机 Java 8 javac（Java 25 runtime 包不含 javac）
#     JAVA_PROCS           JVM ActiveProcessorCount；默认 10，设置 PROCS 时跟随 PROCS
#     JAVA_OPTS            可选 Java 运行参数
#     JAVA_GC_OPTS         可选 Java GC 日志参数；为空时按 JDK 版本自动配置
#
#   用法示例：
#     MSGC_SDK_ROOT=/home/lipeisheng/cangjie/cangjie_compiler/output/msgc \
#     COPYGC_SDK_ROOT=/home/lipeisheng/cangjie-pkg/main/260723 \
#     CASE27_PAYLOAD=10K CASE27_CONCURRENCY=8 \
#     ./run_compare_linux.sh
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

# ---------- 架构检测 ----------
CJ_ARCH="${CJ_ARCH:-$(uname -m)}"
# uname -m 在 Linux 上 aarch64 返回 "aarch64"，x86_64 返回 "x86_64"
PLATFORM_DIR="linux_${CJ_ARCH}_cjnative"

# ---------- SDK 路径（可通过环境变量覆盖） ----------
# MSGC SDK 为单根目录：envsetup.sh / runtime / stdx 平台目录均在其下
MSGC_SDK_ROOT="${MSGC_SDK_ROOT:-/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc}"
MSGC_STDX_ROOT="${MSGC_STDX_ROOT:-$MSGC_SDK_ROOT/$PLATFORM_DIR}"
# CopyGC SDK 为分根目录（260723 解包布局）：
#   - envsetup.sh + runtime 在 260723/cangjie/ 下
#   - linux_<arch>_cjnative 平台目录（含 dynamic/stdx）在 260723/ 顶层
# 因此 COPYGC_SDK_ROOT 指向含 envsetup.sh 的 cangjie/ 子目录，
# COPYGC_STDX_ROOT 指向含 dynamic/stdx 的 linux_<arch>_cjnative/ 平台目录。
COPYGC_SDK_ROOT="${COPYGC_SDK_ROOT:-/home/lipeisheng/cangjie-pkg/main/260723/cangjie}"
COPYGC_STDX_ROOT="${COPYGC_STDX_ROOT:-/home/lipeisheng/cangjie-pkg/main/260723/$PLATFORM_DIR}"

MSGC_SDK="$MSGC_SDK_ROOT"
COPYGC_SDK="$COPYGC_SDK_ROOT"

MSGC_STDX="${MSGC_STDX_ROOT}/dynamic/stdx"
COPYGC_STDX="${COPYGC_STDX_ROOT}/dynamic/stdx"

# ---------- 可选参数（与 README 中的 case27 环境变量对齐） ----------
PAYLOAD="${CASE27_PAYLOAD:-100K}"
CONC="${CASE27_CONCURRENCY:-1000}"
DUR="${CASE27_DURATION_MS:-60000}"
CASE27_NOFILE_LIMIT="${CASE27_NOFILE_LIMIT:-65535}"
HEAP_SIZE="${HEAP_SIZE:-16gb}"
PROCS="${PROCS:-}"
MSGC_HEAP_GROWTH="${MSGC_HEAP_GROWTH:-1}"
MSGC_SCAVENGE_GROWTH="${MSGC_SCAVENGE_GROWTH:-5}"
MSGC_MIN_GC_THRESHOLD="${MSGC_MIN_GC_THRESHOLD:-20mb}"
COPYGC_GC_INTERVAL="${COPYGC_GC_INTERVAL:-}"
MSGC_GC_INTERVAL="${MSGC_GC_INTERVAL:-}"
CLIENT_MODE="${CASE27_CLIENT_MODE:-sync}"
SEND_INTERVAL_MS="${CASE27_SEND_INTERVAL_MS:-1}"
MAX_INFLIGHT="${CASE27_MAX_INFLIGHT:-32}"
GO_ENABLE="${GO_ENABLE:-1}"
GO_GODEBUG="${GO_GODEBUG:-gctrace=1}"
GO_GOGC="${GO_GOGC:-}"
JAVA_ENABLE="${JAVA_ENABLE:-1}"
JAVA_BIN="${JAVA_BIN:-/usr/lib/jvm/java-25-openjdk-arm64/bin/java}"
JAVAC_BIN="${JAVAC_BIN:-/usr/lib/jvm/java-8-openjdk-arm64/bin/javac}"
JAVA_PROCS="${JAVA_PROCS:-${PROCS:-10}}"
JAVA_OPTS="${JAVA_OPTS:-}"
JAVA_GC_OPTS="${JAVA_GC_OPTS:-}"

configure_nofile_limit() {
    local requested="$1"
    local concurrency="$2"
    local required
    local current
    local hard
    local target
    local effective

    if [[ ! "$requested" =~ ^[1-9][0-9]*$ ]]; then
        echo "[ERROR] CASE27_NOFILE_LIMIT must be a positive integer: $requested" >&2
        return 1
    fi
    if [[ ! "$concurrency" =~ ^[1-9][0-9]*$ ]]; then
        echo "[ERROR] CASE27_CONCURRENCY must be a positive integer: $concurrency" >&2
        return 1
    fi

    # sync 模式下每个并发请求至少占用 client/server 两端各一个 socket，
    # 再为监听 socket、日志、runtime 和采样器预留 1024 个 fd。
    required=$((concurrency * 2 + 1024))
    current="$(ulimit -Sn)"
    hard="$(ulimit -Hn)"
    target="$requested"

    if [[ "$hard" != "unlimited" ]]; then
        if [[ ! "$hard" =~ ^[0-9]+$ ]]; then
            echo "[ERROR] unsupported hard nofile limit: $hard" >&2
            return 1
        fi
        if ((target > hard)); then
            echo "[WARN] requested nofile=$target exceeds hard limit=$hard; using hard limit" >&2
            target="$hard"
        fi
    fi

    if [[ "$current" != "unlimited" ]]; then
        if [[ ! "$current" =~ ^[0-9]+$ ]]; then
            echo "[ERROR] unsupported soft nofile limit: $current" >&2
            return 1
        fi
        if ((current < target)); then
            if ! ulimit -Sn "$target"; then
                echo "[ERROR] failed to raise nofile soft limit from $current to $target" >&2
                return 1
            fi
        fi
    fi

    effective="$(ulimit -Sn)"
    if [[ "$effective" != "unlimited" ]]; then
        if [[ ! "$effective" =~ ^[0-9]+$ ]]; then
            echo "[ERROR] unsupported effective nofile limit: $effective" >&2
            return 1
        fi
        if ((effective < required)); then
            echo "[ERROR] effective nofile=$effective is below required=$required for concurrency=$concurrency" >&2
            echo "[ERROR] raise the hard limit or lower CASE27_CONCURRENCY" >&2
            return 1
        fi
    fi

    CASE27_NOFILE_REQUIRED="$required"
    CASE27_NOFILE_EFFECTIVE="$effective"
    echo "[INFO] nofile: requested=$requested required=$required effective=$effective hard=$hard"
}

if ! configure_nofile_limit "$CASE27_NOFILE_LIMIT" "$CONC"; then
    exit 1
fi

if [[ ! -f "$CASE_DIR/payloads/${PAYLOAD}.json" ]]; then
    echo "[ERROR] payloads/${PAYLOAD}.json not found" >&2
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
    if [[ ! -f "$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmark.java" ]] ||
       [[ ! -f "$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmarkTest.java" ]]; then
        echo "[ERROR] Java benchmark sources are incomplete under $CASE_DIR/java_benchmark" >&2
        exit 1
    fi
fi

if [[ ! -d "$MSGC_SDK_ROOT" ]]; then
    echo "[ERROR] MSGC SDK root not found: $MSGC_SDK_ROOT" >&2
    exit 1
fi
if [[ ! -d "$COPYGC_SDK_ROOT" ]]; then
    echo "[ERROR] CopyGC SDK root not found: $COPYGC_SDK_ROOT" >&2
    exit 1
fi

RUN_DATE="$(date +%Y%m%d)"
RESULT_PROCS="${PROCS:-auto}"
RESULT_HEAP_SIZE="${HEAP_SIZE^^}"
COMPARE_DIR="$CASE_DIR/result/${RUN_DATE}_c${CONC}_${PAYLOAD}_HeapGrow${MSGC_HEAP_GROWTH}_ScavengeGrow${MSGC_SCAVENGE_GROWTH}_P${RESULT_PROCS}_HeapSize${RESULT_HEAP_SIZE}_compare"
COPYGC_DIR="$COMPARE_DIR/copygc"
MSGC_DIR="$COMPARE_DIR/msgc"
GO_DIR="$COMPARE_DIR/go"
JAVA_DIR="$COMPARE_DIR/java"
MPLCONFIG_DIR="$SHARED_MPLCONFIG_DIR"
mkdir -p "$COPYGC_DIR" "$MSGC_DIR"
if [[ "$GO_ENABLE" != "0" ]]; then
    mkdir -p "$GO_DIR"
fi
if [[ "$JAVA_ENABLE" != "0" ]]; then
    mkdir -p "$JAVA_DIR"
fi
mkdir -p "$MPLCONFIG_DIR"
echo "[INFO] compare dir: $COMPARE_DIR"
echo "[INFO] platform_dir: $PLATFORM_DIR (arch=$CJ_ARCH)"
echo "[INFO] config: payload=$PAYLOAD concurrency=$CONC duration=${DUR}ms heap=$HEAP_SIZE proc=${PROCS:-auto} mode=$CLIENT_MODE nofile=$CASE27_NOFILE_EFFECTIVE msgcMinGCThreshold=$MSGC_MIN_GC_THRESHOLD"

# Linux 下计算 dylib md5（md5sum 输出 "hash  file"）
dylib_md5() {
    local f="$1"
    if [[ -f "$f" ]]; then
        md5sum "$f" 2>/dev/null | awk '{print $1}'
    else
        echo 'N/A'
    fi
}

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
        echo "cjc_gc_mode=$cjc_gc_mode"
        echo "payload=$PAYLOAD"
        echo "concurrency=$CONC"
        echo "duration_ms=$DUR"
        echo "nofile_requested=$CASE27_NOFILE_LIMIT"
        echo "nofile_required=$CASE27_NOFILE_REQUIRED"
        echo "nofile_effective=$CASE27_NOFILE_EFFECTIVE"
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
        echo "cj_ms_min_gc_threshold=${runtime_min_gc_threshold:-}"
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
        echo "platform_dir=$PLATFORM_DIR"
        echo "arch=$CJ_ARCH"
        echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "runtime_dylib_md5=$(dylib_md5 "$sdk_root"/runtime/lib/$PLATFORM_DIR/libcangjie-runtime.so)"
    } > "$out_dir/run_metadata.env"
    cp "$CASE_DIR/benchmark.cj" "$out_dir/benchmark.cj.snapshot"
    cp "$CASE_DIR/payloads/${PAYLOAD}.json" "$out_dir/payload.snapshot.json"

    local -a child_env=(
        "HOME=$HOME"
        "TERM=${TERM:-xterm}"
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin"
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
            export LD_LIBRARY_PATH="$STDX_DIR:${LD_LIBRARY_PATH:-}"
            echo "[$LABEL] cjc: $(which cjc)"
            echo "[$LABEL] LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
            echo "[$LABEL] cjc gc mode=$CJC_GC_MODE"
            echo "[$LABEL] CJ_MS_DISABLE_ASSIST=${CJ_MS_DISABLE_ASSIST:-<default>}"
            echo "[$LABEL] CJ_MS_VERIFY_MARK=${CJ_MS_VERIFY_MARK:-<default>}"
            echo "[$LABEL] CJ_MS_VERIFY_MARK_DETAIL=${CJ_MS_VERIFY_MARK_DETAIL:-<default>}"
            echo "[$LABEL] CJ_MS_VERIFY_ROOTS=${CJ_MS_VERIFY_ROOTS:-<default>}"
            echo "[$LABEL] cjGCBGSweepMaxTime=${cjGCBGSweepMaxTime:-<default>}"
            echo "[$LABEL] cjScavengeGrowth=${RUNTIME_SCAVENGE_GROWTH:-<default>}"
            echo "[$LABEL] cjMSMinGCThreshold=${RUNTIME_MIN_GC_THRESHOLD:-<default>}"
            echo "[$LABEL] CASE27_BODY_DIAG=${CASE27_BODY_DIAG:-<default>}"
            echo "[$LABEL] CASE27_DUMP_BAD_BODY=${CASE27_DUMP_BAD_BODY:-<default>}"
            echo "[$LABEL] CASE27_DUMP_DIR=${CASE27_DUMP_DIR:-<default>}"
            echo "[$LABEL] CASE27_GRAPH_DIAG=${CASE27_GRAPH_DIAG:-<default>}"
            echo "[$LABEL] CASE27_GRAPH_DIAG_RATE=${CASE27_GRAPH_DIAG_RATE:-<default>}"

            cd "$OUT_DIR"
            cjc_args=("$BENCH_SRC")
            if [ -n "$CJC_GC_MODE" ]; then
                cjc_args+=("--gc-mode=$CJC_GC_MODE")
            fi
            cjc "${cjc_args[@]}" \
                --import-path "$STDX_DIR" \
                -L "$STDX_DIR" \
                --link-option "$STDX_DIR/libstdx.net.http.so" \
                --link-option "$STDX_DIR/libstdx.encoding.json.so" \
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

            if [ "$CJC_GC_MODE" = "marksweep" ]; then
                echo "[$LABEL] cjMSMaxVirtualHeapSize=$HEAP_SIZE"
                export cjMSMaxVirtualHeapSize="$HEAP_SIZE"
            else
                unset cjMSMaxVirtualHeapSize 2>/dev/null || true
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
        echo "nofile_requested=$CASE27_NOFILE_LIMIT"
        echo "nofile_required=$CASE27_NOFILE_REQUIRED"
        echo "nofile_effective=$CASE27_NOFILE_EFFECTIVE"
        echo "client_timeout_ms=${CASE27_CLIENT_TIMEOUT_MS:-5000}"
        echo "client_mode=$CLIENT_MODE"
        echo "send_interval_ms=$SEND_INTERVAL_MS"
        echo "max_inflight=$MAX_INFLIGHT"
        echo "server_port=$port"
        echo "original_port=$orig_port"
        echo "heap_size=N/A"
        echo "procs=${PROCS:-auto}"
        echo "go_gomaxprocs=${PROCS:-auto}"
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
        "PATH=/usr/local/go/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
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
        "PROCS=$PROCS"
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
            if [ -n "${PROCS:-}" ]; then
                export GOMAXPROCS="$PROCS"
            else
                unset GOMAXPROCS 2>/dev/null || true
            fi
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
        echo "case=case27_http_complex_json"
        echo "label=$label"
        echo "payload=$PAYLOAD"
        echo "concurrency=$CONC"
        echo "duration_ms=$DUR"
        echo "nofile_requested=$CASE27_NOFILE_LIMIT"
        echo "nofile_required=$CASE27_NOFILE_REQUIRED"
        echo "nofile_effective=$CASE27_NOFILE_EFFECTIVE"
        echo "client_timeout_ms=${CASE27_CLIENT_TIMEOUT_MS:-5000}"
        echo "client_mode=$CLIENT_MODE"
        echo "send_interval_ms=$SEND_INTERVAL_MS"
        echo "max_inflight=$MAX_INFLIGHT"
        echo "server_port=$port"
        echo "original_port=$orig_port"
        echo "heap_size=N/A"
        echo "procs=${JAVA_PROCS:-auto}"
        echo "java_active_processor_count=${JAVA_PROCS:-auto}"
        echo "java_bin=$JAVA_BIN"
        echo "javac_bin=$JAVAC_BIN"
        echo "java_opts=$JAVA_OPTS"
        echo "java_gc_opts=$JAVA_GC_OPTS"
        echo "java_version=$("$JAVA_BIN" -version 2>&1 | sed -n '1p')"
        echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$out_dir/run_metadata.env"
    cp "$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmark.java" \
        "$out_dir/Case27HttpComplexJsonBenchmark.java.snapshot"
    cp "$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmarkTest.java" \
        "$out_dir/Case27HttpComplexJsonBenchmarkTest.java.snapshot"
    cp "$CASE_DIR/payloads/${PAYLOAD}.json" "$out_dir/payload.snapshot.json"

    local -a child_env=(
        "HOME=$HOME"
        "TERM=${TERM:-xterm}"
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin"
        "CASE_DIR=$CASE_DIR"
        "OUT_DIR=$out_dir"
        "LABEL=$label"
        "JAVA_BIN=$JAVA_BIN"
        "JAVAC_BIN=$JAVAC_BIN"
        "JAVA_PROCS=$JAVA_PROCS"
        "JAVA_OPTS=$JAVA_OPTS"
        "JAVA_GC_OPTS=$JAVA_GC_OPTS"
        "CASE27_PAYLOAD=$PAYLOAD"
        "CASE27_CONCURRENCY=$CONC"
        "CASE27_DURATION_MS=$DUR"
        "CASE27_SERVER_PORT=$port"
        "CASE27_CLIENT_TIMEOUT_MS=${CASE27_CLIENT_TIMEOUT_MS:-5000}"
        "CASE27_PAYLOAD_DIR=$CASE_DIR/payloads"
        "CASE27_CLIENT_MODE=$CLIENT_MODE"
        "CASE27_SEND_INTERVAL_MS=$SEND_INTERVAL_MS"
        "CASE27_MAX_INFLIGHT=$MAX_INFLIGHT"
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
            if [[ -n "$JAVA_PROCS" ]] && [[ ! " $JAVA_OPTS " =~ [[:space:]]-XX:ActiveProcessorCount= ]]; then
                java_opts+=("-XX:ActiveProcessorCount=$JAVA_PROCS")
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
            } > "$OUT_DIR/java_command.env"

            "$JAVAC_BIN" \
                -d "$OUT_DIR/classes" \
                "$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmark.java" \
                "$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmarkTest.java" \
                > "$OUT_DIR/compile.stdout.txt" 2> "$OUT_DIR/compile.stderr.txt"
            compile_rc=$?
            echo "$compile_rc" > "$OUT_DIR/compile.exitcode"
            if ((compile_rc != 0)); then
                echo "[$LABEL] javac failed (rc=$compile_rc)" >&2
                exit "$compile_rc"
            fi

            "$JAVA_BIN" "${module_opts[@]}" "${java_opts[@]}" \
                -cp "$OUT_DIR/classes" Case27HttpComplexJsonBenchmarkTest \
                > "$OUT_DIR/test.stdout.txt" 2> "$OUT_DIR/test.stderr.txt"
            test_rc=$?
            echo "$test_rc" > "$OUT_DIR/test.exitcode"
            if ((test_rc != 0)); then
                echo "[$LABEL] Java benchmark test failed (rc=$test_rc)" >&2
                exit "$test_rc"
            fi

            : > "$OUT_DIR/gc.log"
            "$JAVA_BIN" "${module_opts[@]}" "${java_opts[@]}" "${gc_opts[@]}" \
                -cp "$OUT_DIR/classes" Case27HttpComplexJsonBenchmark \
                > "$OUT_DIR/stdout.txt" 2> "$OUT_DIR/stderr.txt" &
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
            run_rc=$?
            kill "$SAMPLER_PID" 2>/dev/null || true
            wait "$SAMPLER_PID" 2>/dev/null || true
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

# 先跑 CopyGC 基线，再跑 MSGC
run_side "CopyGC" "$COPYGC_DIR" "$COPYGC_SDK" "$COPYGC_STDX" 18226 "" "" "$COPYGC_GC_INTERVAL" "" ""
COPYGC_RC=$?

run_side "MSGC" "$MSGC_DIR" "$MSGC_SDK" "$MSGC_STDX" 18230 \
    "$MSGC_HEAP_GROWTH" "$MSGC_SCAVENGE_GROWTH" "$MSGC_GC_INTERVAL" "$MSGC_MIN_GC_THRESHOLD" "marksweep"
MSGC_RC=$?

echo "[INFO] CopyGC rc=$COPYGC_RC  MSGC rc=$MSGC_RC"
GO_RC=0
if [[ "$GO_ENABLE" != "0" ]]; then
    run_go_side "Go" "$GO_DIR" 18234
    GO_RC=$?
    echo "[INFO] Go rc=$GO_RC"
fi
JAVA_RC=0
if [[ "$JAVA_ENABLE" != "0" ]]; then
    run_java_side "Java" "$JAVA_DIR" 18238
    JAVA_RC=$?
    echo "[INFO] Java rc=$JAVA_RC"
fi

compare_args=(
    --run "CopyGC=$COPYGC_DIR"
    --run "MSGC=$MSGC_DIR"
)
if [[ "$GO_ENABLE" != "0" ]]; then
    compare_args+=(--run "Go=$GO_DIR")
fi
if [[ "$JAVA_ENABLE" != "0" ]]; then
    compare_args+=(--run "Java=$JAVA_DIR")
fi
MPLCONFIGDIR="$MPLCONFIG_DIR" "$PYTHON_BIN" "$SCRIPT_DIR/compare_results.py" \
    "${compare_args[@]}" \
    --output "$COMPARE_DIR/compare_report.md"
COMPARE_RC=$?
echo "[INFO] compare_results.py rc=$COMPARE_RC"

echo "[DONE] compare_dir=$COMPARE_DIR"
if [[ "$JAVA_RC" -ne 0 ]]; then
    exit "$JAVA_RC"
fi
