#!/usr/bin/env bash
# Linux runner for case25_http_complex_json_stream.
# This file is intentionally independent from the macOS run_compare.sh.
set -uo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_DIR="$CASE_DIR/script"
PYTHON_BIN="${PYTHON_BIN:-python3}"
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    x86_64|amd64)
        CJ_ARCH="x86_64"
        JAVA_PACKAGE_ARCH="amd64"
        ;;
    aarch64|arm64)
        CJ_ARCH="aarch64"
        JAVA_PACKAGE_ARCH="arm64"
        ;;
    *)
        echo "[ERROR] unsupported Linux architecture: $HOST_ARCH" >&2
        exit 1
        ;;
esac
PLATFORM_DIR="linux_${CJ_ARCH}_cjnative"
JAVA_HOME_DEFAULT="/usr/lib/jvm/java-25-openjdk-${JAVA_PACKAGE_ARCH}"

MSGC_SDK_ROOT="${MSGC_SDK_ROOT:-/home/lipeisheng/cangjie/cangjie_compiler_CMSGC/output/msgc}"
MSGC_STDX_ROOT="${MSGC_STDX_ROOT:-$MSGC_SDK_ROOT/$PLATFORM_DIR}"
if [[ -z "${COPYGC_PACKAGE_ROOT:-}" ]]; then
    case "$CJ_ARCH" in
        x86_64)
            copygc_package_candidates=(
                "/home/lipeisheng/cangjie-pkg/260723"
                "/home/lipeisheng/cangjie-pkg/main/260723"
            )
            ;;
        aarch64)
            copygc_package_candidates=(
                "/home/lipeisheng/cangjie-pkg/main/260723"
                "/home/lipeisheng/cangjie-pkg/260723"
            )
            ;;
    esac
    COPYGC_PACKAGE_ROOT="${copygc_package_candidates[0]}"
    for candidate in "${copygc_package_candidates[@]}"; do
        if [[ -f "$candidate/cangjie/envsetup.sh" &&
              -f "$candidate/$PLATFORM_DIR/dynamic/stdx/libstdx.net.http.so" &&
              -f "$candidate/cangjie/runtime/lib/$PLATFORM_DIR/libcangjie-runtime.so" ]]; then
            COPYGC_PACKAGE_ROOT="$candidate"
            break
        fi
    done
fi
COPYGC_SDK_ROOT="${COPYGC_SDK_ROOT:-$COPYGC_PACKAGE_ROOT/cangjie}"
COPYGC_STDX_ROOT="${COPYGC_STDX_ROOT:-$COPYGC_PACKAGE_ROOT/$PLATFORM_DIR}"
MSGC_STDX="$MSGC_STDX_ROOT/dynamic/stdx"
COPYGC_STDX="$COPYGC_STDX_ROOT/dynamic/stdx"

PAYLOAD="${CASE25_PAYLOAD:-100K}"
CONC="${CASE25_CONCURRENCY:-1000}"
DUR="${CASE25_DURATION_MS:-60000}"
HEAP_SIZE="${HEAP_SIZE:-16gb}"
PROCS="${PROCS:-}"
MSGC_HEAP_GROWTH="${MSGC_HEAP_GROWTH:-1}"
MSGC_SCAVENGE_GROWTH="${MSGC_SCAVENGE_GROWTH:-1.10}"
MSGC_MIN_GC_THRESHOLD="${MSGC_MIN_GC_THRESHOLD:-20mb}"
CLIENT_MODE="${CASE25_CLIENT_MODE:-sync}"
SEND_INTERVAL_MS="${CASE25_SEND_INTERVAL_MS:-1}"
MAX_INFLIGHT="${CASE25_MAX_INFLIGHT:-32}"
PERSISTENT_BYTES_INPUT="${CASE25_PERSISTENT_BYTES:-0}"
PERSISTENT_WORKERS="${CASE25_PERSISTENT_WORKERS:-8}"
COPYGC_ENABLE="${COPYGC_ENABLE:-1}"
MSGC_ENABLE="${MSGC_ENABLE:-1}"
GO_ENABLE="${GO_ENABLE:-1}"
JAVA_ENABLE="${JAVA_ENABLE:-1}"
GO_GODEBUG="${GO_GODEBUG:-gctrace=1}"
GO_GOGC="${GO_GOGC:-}"
GO_GOMEMLIMIT="${GO_GOMEMLIMIT:-16GiB}"
GO_BIN="${GO_BIN:-$(command -v go 2>/dev/null || true)}"
JAVA_BIN="${JAVA_BIN:-$JAVA_HOME_DEFAULT/bin/java}"
JAVAC_BIN="${JAVAC_BIN:-$JAVA_HOME_DEFAULT/bin/javac}"
JACKSON_CORE_JAR="$CASE_DIR/java_benchmark/lib/jackson-core-2.17.2.jar"
JAVA_CPUSET="${JAVA_CPUSET:-}"

if [[ -n "$PROCS" && -z "$JAVA_CPUSET" ]]; then
    if ! [[ "$PROCS" =~ ^[1-9][0-9]*$ ]]; then
        echo "[ERROR] PROCS must be a positive integer when set, got: $PROCS" >&2
        exit 1
    fi
    ONLINE_CPUS="$(getconf _NPROCESSORS_ONLN)"
    if ((PROCS > ONLINE_CPUS)); then
        echo "[ERROR] PROCS=$PROCS exceeds online CPUs=$ONLINE_CPUS" >&2
        exit 1
    fi
    JAVA_CPUSET="0-$((PROCS - 1))"
fi

if ! PERSISTENT_BYTES="$(bash "$SCRIPT_DIR/normalize_byte_size.sh" "$PERSISTENT_BYTES_INPUT")"; then
    exit 1
fi
if [[ ! -f "$CASE_DIR/payloads/${PAYLOAD}.json" ]]; then
    echo "[ERROR] payloads/${PAYLOAD}.json not found" >&2
    exit 1
fi
check_cangjie_sdk() {
    local label="$1" sdk="$2" stdx_root="$3"
    local stdx="$stdx_root/dynamic/stdx"
    local missing=()
    [[ -f "$sdk/envsetup.sh" ]] || missing+=("$sdk/envsetup.sh")
    [[ -f "$stdx/libstdx.net.http.so" ]] || missing+=("$stdx/libstdx.net.http.so")
    [[ -f "$stdx/libstdx.encoding.json.so" ]] || missing+=("$stdx/libstdx.encoding.json.so")
    [[ -f "$stdx/libstdx.encoding.json.stream.so" ]] || missing+=("$stdx/libstdx.encoding.json.stream.so")
    [[ -f "$sdk/runtime/lib/$PLATFORM_DIR/libcangjie-runtime.so" ]] || \
        missing+=("$sdk/runtime/lib/$PLATFORM_DIR/libcangjie-runtime.so")
    if ((${#missing[@]} > 0)); then
        echo "[WARN] skipping $label: SDK dependency unavailable for arch=$CJ_ARCH" >&2
        printf '       missing: %s\n' "${missing[@]}" >&2
        return 1
    fi
    return 0
}

check_java_tool() {
    local tool="$1"
    if [[ "$tool" == */* ]]; then
        [[ -x "$tool" ]]
    else
        command -v "$tool" >/dev/null 2>&1
    fi
}

# Each side is optional.  A missing SDK/tool disables only that side so that
# the other available implementations can still be compared.
if [[ "$COPYGC_ENABLE" == 1 ]] && ! check_cangjie_sdk CopyGC "$COPYGC_SDK_ROOT" "$COPYGC_STDX_ROOT"; then
    COPYGC_ENABLE=0
fi
if [[ "$MSGC_ENABLE" == 1 ]] && ! check_cangjie_sdk MSGC "$MSGC_SDK_ROOT" "$MSGC_STDX_ROOT"; then
    MSGC_ENABLE=0
fi
if [[ "$GO_ENABLE" == 1 && ! -x "$GO_BIN" ]]; then
    echo "[WARN] skipping Go: GO_BIN is unavailable or not executable: $GO_BIN" >&2
    GO_ENABLE=0
fi
if [[ "$JAVA_ENABLE" == 1 ]]; then
    if ! check_java_tool "$JAVA_BIN"; then
        echo "[WARN] skipping Java: JAVA_BIN is unavailable or not executable: $JAVA_BIN" >&2
        JAVA_ENABLE=0
    elif ! check_java_tool "$JAVAC_BIN"; then
        echo "[WARN] skipping Java: JAVAC_BIN is unavailable or not executable: $JAVAC_BIN" >&2
        JAVA_ENABLE=0
    elif [[ -n "$JAVA_CPUSET" ]] && ! command -v taskset >/dev/null 2>&1; then
        echo "[WARN] skipping Java: Java CPU affinity requires taskset, but taskset is unavailable" >&2
        JAVA_ENABLE=0
    elif [[ ! -f "$JACKSON_CORE_JAR" ]]; then
        echo "[WARN] skipping Java: Jackson Core jar is unavailable: $JACKSON_CORE_JAR" >&2
        JAVA_ENABLE=0
    fi
fi
if [[ "$COPYGC_ENABLE" != 1 && "$MSGC_ENABLE" != 1 && "$GO_ENABLE" != 1 && "$JAVA_ENABLE" != 1 ]]; then
    echo "[ERROR] no runnable implementation remains after dependency checks" >&2
    exit 1
fi

TS="$(date +%Y%m%d)"
PAYLOAD_TAG="$(printf '%s' "$PAYLOAD" | tr '[:upper:]' '[:lower:]')"
PERSISTENT_BYTES_MIB=$((PERSISTENT_BYTES / 1024 / 1024))
PROCS_TAG="${PROCS:-Auto}"
BASE="$CASE_DIR/result/${TS}_${PERSISTENT_BYTES_MIB}M_c${CONC}_${PAYLOAD_TAG}_HeapGrow${MSGC_HEAP_GROWTH}_Scavenge${MSGC_SCAVENGE_GROWTH}_Procs${PROCS_TAG}_compare"
COMPARE_DIR="$BASE"
suffix=1
while [[ -e "$COMPARE_DIR" ]]; do
    COMPARE_DIR="${BASE}_${suffix}"
    suffix=$((suffix + 1))
done
mkdir -p "$COMPARE_DIR" "$CASE_DIR/result/.mplconfig"
COPYGC_DIR="$COMPARE_DIR/copygc"
MSGC_DIR="$COMPARE_DIR/msgc"
GO_DIR="$COMPARE_DIR/go"
JAVA_DIR="$COMPARE_DIR/java"
for side in copygc msgc go java; do
    enabled="${side^^}_ENABLE"
    [[ "${!enabled}" == 1 ]] && mkdir -p "$COMPARE_DIR/$side"
done

echo "[INFO] compare dir: $COMPARE_DIR"
echo "[INFO] platform: $PLATFORM_DIR arch=$CJ_ARCH"
echo "[INFO] config: payload=$PAYLOAD concurrency=$CONC duration=${DUR}ms heap=$HEAP_SIZE persistent=${PERSISTENT_BYTES_INPUT} proc=${PROCS:-auto}"
echo "[INFO] SDK MSGC=$MSGC_SDK_ROOT"
echo "[INFO] SDK CopyGC=$COPYGC_SDK_ROOT"
echo "[INFO] Go=$GO_BIN Java=$JAVA_BIN javac=$JAVAC_BIN"
[[ -n "$JAVA_CPUSET" ]] && echo "[INFO] Java CPU affinity=$JAVA_CPUSET"

write_common_metadata() {
    local file="$1" label="$2" sdk="$3" stdx="$4" port="$5"
    {
        echo "case=case25_http_complex_json_stream"
        echo "label=$label"
        echo "sdk_root=$sdk"
        echo "stdx_dir=$stdx"
        echo "platform_dir=$PLATFORM_DIR"
        echo "payload=$PAYLOAD"
        echo "concurrency=$CONC"
        echo "duration_ms=$DUR"
        echo "persistent_bytes=$PERSISTENT_BYTES"
        echo "persistent_workers=$PERSISTENT_WORKERS"
        echo "client_mode=$CLIENT_MODE"
        echo "send_interval_ms=$SEND_INTERVAL_MS"
        echo "max_inflight=$MAX_INFLIGHT"
        echo "server_port=$port"
        echo "heap_size=$HEAP_SIZE"
        echo "procs=$PROCS"
        echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$file"
}

run_cangjie() {
    local label="$1" out="$2" sdk="$3" stdx_root="$4" port="$5" mode="$6"
    local stdx="$stdx_root/dynamic/stdx"
    local transform_src="$CASE_DIR/${label,,}/stream_transform.cj"
    local growth="$7" scavenge="$8" min_threshold="$9"
    write_common_metadata "$out/run_metadata.env" "$label" "$sdk" "$stdx_root" "$port"
    {
        echo "cjc_gc_mode=$mode"
        echo "cj_heap_growth=$growth"
        echo "cj_scavenge_growth=$scavenge"
        echo "cj_ms_min_gc_threshold=$min_threshold"
        echo "runtime_so_md5=$(md5sum "$sdk/runtime/lib/$PLATFORM_DIR/libcangjie-runtime.so" 2>/dev/null | awk '{print $1}' || echo N/A)"
    } >> "$out/run_metadata.env"
    cp "$CASE_DIR/benchmark.cj" "$out/benchmark.cj.snapshot"
    cp "$CASE_DIR/payloads/${PAYLOAD}.json" "$out/payload.snapshot.json"
    cp "$transform_src" "$out/stream_transform.cj.snapshot"

    local -a envs=(
        "HOME=$HOME" "TERM=${TERM:-xterm}"
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin"
        "SDK_ROOT=$sdk" "STDX_DIR=$stdx" "OUT_DIR=$out" "LABEL=$label"
        "TRANSFORM_SRC=$transform_src" "PLATFORM_DIR=$PLATFORM_DIR"
        "CASE_DIR=$CASE_DIR" "HEAP_SIZE=$HEAP_SIZE" "PROCS=$PROCS"
        "RUNTIME_HEAP_GROWTH=$growth" "RUNTIME_SCAVENGE_GROWTH=$scavenge"
        "RUNTIME_MIN_GC_THRESHOLD=$min_threshold" "CJC_GC_MODE=$mode"
        "CASE25_PAYLOAD=$PAYLOAD" "CASE25_CONCURRENCY=$CONC"
        "CASE25_DURATION_MS=$DUR" "CASE25_PERSISTENT_BYTES=$PERSISTENT_BYTES"
        "CASE25_PERSISTENT_WORKERS=$PERSISTENT_WORKERS" "CASE25_SERVER_PORT=$port"
        "CASE25_PAYLOAD_DIR=$CASE_DIR/payloads" "CASE25_CLIENT_MODE=$CLIENT_MODE"
        "CASE25_SEND_INTERVAL_MS=$SEND_INTERVAL_MS" "CASE25_MAX_INFLIGHT=$MAX_INFLIGHT"
    )
    env -i "${envs[@]}" bash -c '
        set +e
        set +u
        source "$SDK_ROOT/envsetup.sh" >"$OUT_DIR/envsetup.stdout.txt" 2>"$OUT_DIR/envsetup.stderr.txt"
        source_rc=$?
        set -u
        if ((source_rc != 0)); then echo "$source_rc" >"$OUT_DIR/envsetup.exitcode"; exit "$source_rc"; fi
        export PATH="$SDK_ROOT/bin:$SDK_ROOT/tools/bin:$PATH"
        export LD_LIBRARY_PATH="$STDX_DIR:$SDK_ROOT/runtime/lib/$PLATFORM_DIR:$SDK_ROOT/tools/lib:${LD_LIBRARY_PATH:-}"
        cd "$OUT_DIR"
        cjc_args=(-O2 "$CASE_DIR/benchmark.cj" "$TRANSFORM_SRC" "$CASE_DIR/persistent_baseline.cj")
        [[ -n "$CJC_GC_MODE" ]] && cjc_args+=("--gc-mode=$CJC_GC_MODE")
        cjc "${cjc_args[@]}" --import-path "$STDX_DIR" -L "$STDX_DIR" \
            --link-option "$STDX_DIR/libstdx.net.http.so" \
            --link-option "$STDX_DIR/libstdx.encoding.json.so" \
            --link-option "$STDX_DIR/libstdx.encoding.json.stream.so" \
            -o benchmark >compile.stdout.txt 2>compile.stderr.txt
        compile_rc=$?; echo "$compile_rc" >compile.exitcode
        if ((compile_rc != 0)) || [[ ! -x benchmark ]]; then exit "${compile_rc:-1}"; fi
        export cjHeapSize="$HEAP_SIZE"
        if [[ -n "$PROCS" ]]; then export cjProcessorNum="$PROCS"; else unset cjProcessorNum; fi
        if [[ -n "$RUNTIME_HEAP_GROWTH" ]]; then export cjHeapGrowth="$RUNTIME_HEAP_GROWTH"; else unset cjHeapGrowth; fi
        if [[ -n "$RUNTIME_SCAVENGE_GROWTH" ]]; then export cjScavengeGrowth="$RUNTIME_SCAVENGE_GROWTH"; else unset cjScavengeGrowth; fi
        if [[ -n "$RUNTIME_MIN_GC_THRESHOLD" ]]; then export cjMSMinGCThreshold="$RUNTIME_MIN_GC_THRESHOLD"; else unset cjMSMinGCThreshold; fi
        MRT_REPORT="$OUT_DIR/gc.log" MRT_LOG_FILE_SIZE=200mb ./benchmark >stdout.txt 2>stderr.txt &
        pid=$!
        (while kill -0 "$pid" 2>/dev/null; do
            timestamp=$(date -u +%Y-%m-%dT%H:%M:%S.%N)
            timestamp="${timestamp:0:26}"
            ps -o rss=,%cpu= -p "$pid" 2>/dev/null | awk -v t="$timestamp" "NF { print t \",\" \$1 \",\" \$2 }"
            sleep 0.2
        done) >rss.csv & sampler=$!
        wait "$pid"; run_rc=$?; kill "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null
        echo "$run_rc" >run.exitcode; exit "$run_rc"
    '
    local rc=$?
    echo "[INFO] $label rc=$rc"
    return "$rc"
}

run_go() {
    local out="$1" port="$2"
    write_common_metadata "$out/run_metadata.env" Go N/A N/A "$port"
    {
        echo "go_bin=$GO_BIN"; echo "go_version=$("$GO_BIN" version 2>&1)"
        echo "gomaxprocs=$PROCS"; echo "go_godebug=$GO_GODEBUG"; echo "go_gomemlimit=$GO_GOMEMLIMIT"
    } >> "$out/run_metadata.env"
    cp "$CASE_DIR/payloads/${PAYLOAD}.json" "$out/payload.snapshot.json"
    local -a envs=("HOME=$HOME" "TERM=${TERM:-xterm}" "PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin"
        "CASE_DIR=$CASE_DIR" "OUT_DIR=$out" "CASE25_PAYLOAD=$PAYLOAD" "CASE25_CONCURRENCY=$CONC"
        "CASE25_DURATION_MS=$DUR" "CASE25_PERSISTENT_BYTES=$PERSISTENT_BYTES" "CASE25_PERSISTENT_WORKERS=$PERSISTENT_WORKERS"
        "CASE25_SERVER_PORT=$port" "CASE25_PAYLOAD_DIR=$CASE_DIR/payloads" "CASE25_CLIENT_MODE=$CLIENT_MODE"
        "CASE25_SEND_INTERVAL_MS=$SEND_INTERVAL_MS" "CASE25_MAX_INFLIGHT=$MAX_INFLIGHT" "GO_BIN=$GO_BIN"
        "GO_GODEBUG=$GO_GODEBUG" "GO_GOGC=$GO_GOGC" "GOMEMLIMIT=$GO_GOMEMLIMIT" "PROCS=$PROCS")
    env -i "${envs[@]}" bash -c '
        set +e
        export GOCACHE="$OUT_DIR/.gocache" GOTMPDIR="$OUT_DIR/.gotmp"; mkdir -p "$GOCACHE" "$GOTMPDIR"
        cd "$CASE_DIR/go_benchmark"
        "$GO_BIN" build -o "$OUT_DIR/benchmark_go" . >"$OUT_DIR/compile.stdout.txt" 2>"$OUT_DIR/compile.stderr.txt"
        compile_rc=$?; echo "$compile_rc" >"$OUT_DIR/compile.exitcode"; ((compile_rc == 0)) || exit "$compile_rc"
        # CASE27_PAYLOAD=$PAYLOAD is a compatibility bridge for the shared Go implementation.
        export CASE27_PAYLOAD="$CASE25_PAYLOAD" CASE27_CONCURRENCY="$CASE25_CONCURRENCY" CASE27_DURATION_MS="$CASE25_DURATION_MS"
        export CASE27_PERSISTENT_BYTES="$CASE25_PERSISTENT_BYTES" CASE27_PERSISTENT_WORKERS="$CASE25_PERSISTENT_WORKERS"
        export CASE27_SERVER_PORT="$CASE25_SERVER_PORT" CASE27_PAYLOAD_DIR="$CASE25_PAYLOAD_DIR" CASE27_CLIENT_MODE="$CASE25_CLIENT_MODE"
        export CASE27_SEND_INTERVAL_MS="$CASE25_SEND_INTERVAL_MS" CASE27_MAX_INFLIGHT="$CASE25_MAX_INFLIGHT"
        [[ -n "$PROCS" ]] && export GOMAXPROCS="$PROCS"
        if [[ -n "$GO_GOGC" ]]; then export GOGC="$GO_GOGC"; else unset GOGC; fi
        cd "$OUT_DIR"
        GODEBUG="$GO_GODEBUG" ./benchmark_go >stdout.txt 2>gc.log &
        pid=$!
        (while kill -0 "$pid" 2>/dev/null; do
            timestamp=$(date -u +%Y-%m-%dT%H:%M:%S.%N)
            timestamp="${timestamp:0:26}"
            ps -o rss=,%cpu= -p "$pid" 2>/dev/null | awk -v t="$timestamp" "NF { print t \",\" \$1 \",\" \$2 }"
            sleep 0.2
        done) >rss.csv & sampler=$!
        wait "$pid"; run_rc=$?
        kill "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null
        echo "$run_rc" >run.exitcode; exit "$run_rc"
    '
    local rc=$?; echo "[INFO] Go rc=$rc"; return "$rc"
}

run_java_side() {
    local label="$1" out="$2" port="$3"
    write_common_metadata "$out/run_metadata.env" "$label" N/A N/A "$port"
    cp "$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmark.java" "$out/Case27HttpComplexJsonBenchmark.java.snapshot"
    cp "$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmarkTest.java" "$out/Case27HttpComplexJsonBenchmarkTest.java.snapshot"
    cp "$JACKSON_CORE_JAR" "$out/jackson-core-2.17.2.jar.snapshot"
    cp "$CASE_DIR/payloads/${PAYLOAD}.json" "$out/payload.snapshot.json"
    {
        echo "java_bin=$JAVA_BIN"; echo "javac_bin=$JAVAC_BIN"; echo "java_opts=-Xmx${HEAP_SIZE%?}"
        echo "java_active_processor_count=${PROCS:-host-default}"; echo "java_cpuset=${JAVA_CPUSET:-unrestricted}"
        echo "java_version=$("$JAVA_BIN" -version 2>&1 | sed -n '1p')"
    } >> "$out/run_metadata.env"
    local java_heap="${HEAP_SIZE,,}"
    case "$java_heap" in
        *gb) java_heap="${java_heap%gb}g" ;;
        *mb) java_heap="${java_heap%mb}m" ;;
        *kb) java_heap="${java_heap%kb}k" ;;
    esac
    env -i HOME="$HOME" TERM="${TERM:-xterm}" PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        CASE_DIR="$CASE_DIR" OUT_DIR="$out" JAVA_BIN="$JAVA_BIN" JAVAC_BIN="$JAVAC_BIN" JACKSON_CORE_JAR="$JACKSON_CORE_JAR" \
        JAVA_HEAP="$java_heap" CASE25_PAYLOAD="$PAYLOAD" CASE25_CONCURRENCY="$CONC" CASE25_DURATION_MS="$DUR" \
        CASE25_PERSISTENT_BYTES="$PERSISTENT_BYTES" CASE25_PERSISTENT_WORKERS="$PERSISTENT_WORKERS" CASE25_SERVER_PORT="$port" \
        CASE25_PAYLOAD_DIR="$CASE_DIR/payloads" CASE25_CLIENT_MODE="$CLIENT_MODE" CASE25_SEND_INTERVAL_MS="$SEND_INTERVAL_MS" CASE25_MAX_INFLIGHT="$MAX_INFLIGHT" PROCS="$PROCS" JAVA_CPUSET="$JAVA_CPUSET" \
        bash -c '
        set +e; mkdir -p "$OUT_DIR/classes"
        version="$($JAVA_BIN -version 2>&1 | sed -n "1p")"; token="$(printf "%s" "$version" | sed -E "s/.*version \\\"([^\\\"]+)\\\".*/\\1/")"
        major="${token#1.}"; [[ "$token" != 1.* ]] && major="${token%%.*}"
        module_opts=(); ((major >= 9)) && module_opts=(--add-modules jdk.httpserver)
        cpu_opts=(); [[ -n "$PROCS" ]] && cpu_opts=(-XX:ActiveProcessorCount="$PROCS")
        java_cmd=("$JAVA_BIN")
        if [[ -n "$JAVA_CPUSET" ]]; then
            java_cmd=(taskset -c "$JAVA_CPUSET" "${java_cmd[@]}")
        fi
        # jdk.httpserver is a runtime module; javac only needs the Jackson classpath.
        "$JAVAC_BIN" -cp "$JACKSON_CORE_JAR" -d "$OUT_DIR/classes" \
            "$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmark.java" "$CASE_DIR/java_benchmark/Case27HttpComplexJsonBenchmarkTest.java" \
            >"$OUT_DIR/compile.stdout.txt" 2>"$OUT_DIR/compile.stderr.txt"
        rc=$?; echo "$rc" >"$OUT_DIR/compile.exitcode"; ((rc == 0)) || exit "$rc"
        # CASE27_PAYLOAD=$PAYLOAD is a compatibility bridge for the shared Java implementation.
        export CASE27_PAYLOAD="$CASE25_PAYLOAD" CASE27_CONCURRENCY="$CASE25_CONCURRENCY" CASE27_DURATION_MS="$CASE25_DURATION_MS"
        export CASE27_PERSISTENT_BYTES="$CASE25_PERSISTENT_BYTES" CASE27_PERSISTENT_WORKERS="$CASE25_PERSISTENT_WORKERS" CASE27_SERVER_PORT="$CASE25_SERVER_PORT" CASE27_PAYLOAD_DIR="$CASE25_PAYLOAD_DIR" CASE27_CLIENT_MODE="$CASE25_CLIENT_MODE" CASE27_SEND_INTERVAL_MS="$CASE25_SEND_INTERVAL_MS" CASE27_MAX_INFLIGHT="$CASE25_MAX_INFLIGHT"
        "${java_cmd[@]}" "${module_opts[@]}" "${cpu_opts[@]}" -Xmx"$JAVA_HEAP" -cp "$OUT_DIR/classes:$JACKSON_CORE_JAR" Case27HttpComplexJsonBenchmarkTest >"$OUT_DIR/test.stdout.txt" 2>"$OUT_DIR/test.stderr.txt"
        rc=$?; echo "$rc" >"$OUT_DIR/test.exitcode"; ((rc == 0)) || exit "$rc"
        : >"$OUT_DIR/gc.log"
        "${java_cmd[@]}" "${module_opts[@]}" "${cpu_opts[@]}" -Xmx"$JAVA_HEAP" -Xlog:gc*:file="$OUT_DIR/gc.log":time,uptime,level,tags -cp "$OUT_DIR/classes:$JACKSON_CORE_JAR" Case27HttpComplexJsonBenchmark >"$OUT_DIR/stdout.txt" 2>"$OUT_DIR/stderr.txt" &
        pid=$!
        (while kill -0 "$pid" 2>/dev/null; do
            timestamp=$(date -u +%Y-%m-%dT%H:%M:%S.%N)
            timestamp="${timestamp:0:26}"
            ps -o rss=,%cpu= -p "$pid" 2>/dev/null | awk -v t="$timestamp" "NF { print t \",\" \$1 \",\" \$2 }"
            sleep 0.2
        done) >"$OUT_DIR/rss.csv" & sampler=$!
        wait "$pid"; rc=$?
        kill "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null
        echo "$rc" >"$OUT_DIR/run.exitcode"; exit "$rc"
        '
    local rc=$?; echo "[INFO] Java rc=$rc"; return "$rc"
}

COPYGC_RC=0; MSGC_RC=0; GO_RC=0; JAVA_RC=0
if [[ "$COPYGC_ENABLE" == 1 ]]; then
    run_cangjie CopyGC "$COPYGC_DIR" "$COPYGC_SDK_ROOT" "$COPYGC_STDX_ROOT" 18226 "" "" "" "" || COPYGC_RC=$?
fi
if [[ "$MSGC_ENABLE" == 1 ]]; then
    run_cangjie MSGC "$MSGC_DIR" "$MSGC_SDK_ROOT" "$MSGC_STDX_ROOT" 18230 marksweep "$MSGC_HEAP_GROWTH" "$MSGC_SCAVENGE_GROWTH" "$MSGC_MIN_GC_THRESHOLD" || MSGC_RC=$?
fi
if [[ "$GO_ENABLE" == 1 ]]; then
    run_go "$GO_DIR" 18234 || GO_RC=$?
fi
if [[ "$JAVA_ENABLE" == 1 ]]; then
    run_java_side "Java" "$JAVA_DIR" 18238 || JAVA_RC=$?
fi

compare_args=()
[[ "$COPYGC_ENABLE" == 1 ]] && compare_args+=(--run "CopyGC=$COPYGC_DIR")
[[ "$MSGC_ENABLE" == 1 ]] && compare_args+=(--run "MSGC=$MSGC_DIR")
[[ "$GO_ENABLE" == 1 ]] && compare_args+=(--run "Go=$GO_DIR")
[[ "$JAVA_ENABLE" == 1 ]] && compare_args+=(--run "Java=$JAVA_DIR")
if ((${#compare_args[@]} >= 2)); then
    MPLCONFIGDIR="$CASE_DIR/result/.mplconfig" "$PYTHON_BIN" "$SCRIPT_DIR/compare_results.py" \
        "${compare_args[@]}" --output "$COMPARE_DIR/compare_report.md" || true
fi
echo "[DONE] compare_dir=$COMPARE_DIR"
if [[ "$JAVA_RC" -ne 0 ]]; then
    exit "$JAVA_RC"
fi
for rc in "$COPYGC_RC" "$MSGC_RC" "$GO_RC"; do
    [[ "$rc" -ne 0 ]] && exit "$rc"
done
exit 0
