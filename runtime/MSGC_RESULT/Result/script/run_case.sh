#!/usr/bin/env bash
# ============================================================
#  run_case.sh — 构建 runtime → 替换 SDK dylib → 编译用例 → 运行采集
#
#  用法：
#    bash MSGC_RESULT/Result/script/run_case.sh \
#      --case case1_baseline --gc-mode marksweep --tag my_run
#
#  必须在 runtime 根目录下执行。
# ============================================================

set -euo pipefail

# ---------- 路径常量 ----------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULT_BASE="$(cd "$SCRIPT_DIR/.." && pwd)"
RUNTIME_ROOT="$(cd "$RESULT_BASE/../.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-$RUNTIME_ROOT/.venv/bin/python}"
SDK_ROOT="${SDK_ROOT:-/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie}"
SDK_ENV="$SDK_ROOT/envsetup.sh"
SDK_LIB_DIR="$SDK_ROOT/runtime/lib/darwin_aarch64_cjnative"
SDK_DYLIB="$SDK_LIB_DIR/libcangjie-runtime.dylib"
SDK_DYLIB_ORIGIN="$SDK_LIB_DIR/libcangjie-runtime.dylib.origin"
BUILD_DYLIB="$RUNTIME_ROOT/output/temp/lib/aarch64_Release/libcangjie-runtime.dylib"
RSS_MONITOR="$RUNTIME_ROOT/MSGC/benchmark/rss_monitor.py"
ANALYZER="$SCRIPT_DIR/analyze_case.py"

# ---------- 默认参数 ----------
CASE_NAME=""
GC_MODE=""        # copygc | marksweep | compare
RUN_TAG=""
HEAP_SIZE="256mb"
SAMPLE_INTERVAL_MS="200"
SKIP_BUILD=0
SEED_NAME=""
declare -a EXTRA_ENV_ARGS=()
declare -a SEED_ENV_ARGS=()

# ---------- 工具函数 ----------
info() { printf '[INFO] %s\n' "$*" >&2; }
fail() { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Usage: run_case.sh [options]

Options:
  --case NAME              用例目录名 (如 case1_baseline)
  --gc-mode MODE           copygc | marksweep | compare
  --tag TAG                可选标签，追加到结果目录名
  --seed NAME              加载 seed/<NAME>.env 中的参数 (仅作用于 MSGC)
  --heap-size SIZE         堆大小 (默认: 256mb)
  --sample-interval-ms N   RSS/CPU 采样间隔 (默认: 200)
  --skip-build             跳过 runtime 构建 (使用已有产物)
  --env KEY=VALUE          传递额外环境变量 (可重复)
  -h, --help               显示帮助
EOF
    exit 0
}

# ---------- 参数解析 ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --case)          CASE_NAME="$2"; shift 2 ;;
        --gc-mode)       GC_MODE="$2"; shift 2 ;;
        --tag)           RUN_TAG="$2"; shift 2 ;;
        --heap-size)     HEAP_SIZE="$2"; shift 2 ;;
        --sample-interval-ms) SAMPLE_INTERVAL_MS="$2"; shift 2 ;;
        --skip-build)    SKIP_BUILD=1; shift ;;
        --seed)          SEED_NAME="$2"; shift 2 ;;
        --env)           EXTRA_ENV_ARGS+=("$2"); shift 2 ;;
        -h|--help)       usage ;;
        *)               fail "Unknown option: $1" ;;
    esac
done

[[ -n "$CASE_NAME" ]] || fail "必须指定 --case"
[[ -n "$GC_MODE" ]]   || fail "必须指定 --gc-mode (copygc | marksweep | compare)"

CASE_DIR="$RESULT_BASE/$CASE_NAME"
BENCHMARK_SRC="$CASE_DIR/benchmark.cj"
[[ -d "$CASE_DIR" ]]       || fail "用例目录不存在: $CASE_DIR"
[[ -f "$BENCHMARK_SRC" ]]  || fail "用例源文件不存在: $BENCHMARK_SRC"

# ---------- 加载 seed 文件 ----------
load_seed() {
    if [[ -z "$SEED_NAME" ]]; then
        return
    fi
    local seed_file="$RESULT_BASE/seed/${SEED_NAME}.env"
    [[ -f "$seed_file" ]] || fail "seed 文件不存在: $seed_file"
    info "加载 seed: $seed_file"
    while IFS= read -r line || [[ -n "$line" ]]; do
        # 跳过空行和注释
        [[ -z "$line" || "$line" == \#* ]] && continue
        SEED_ENV_ARGS+=("$line")
    done < "$seed_file"
}

# ---------- 环境变量导出 ----------
export_extra_env() {
    for kv in "${EXTRA_ENV_ARGS[@]+"${EXTRA_ENV_ARGS[@]}"}"; do
        export "$kv"
    done
}

# 导出 seed + extra 环境变量（用于 MSGC）
export_seed_and_extra_env() {
    for kv in "${SEED_ENV_ARGS[@]+"${SEED_ENV_ARGS[@]}"}"; do
        export "$kv"
    done
    for kv in "${EXTRA_ENV_ARGS[@]+"${EXTRA_ENV_ARGS[@]}"}"; do
        export "$kv"
    done
}

# 清除 seed 环境变量
unset_seed_env() {
    for kv in "${SEED_ENV_ARGS[@]+"${SEED_ENV_ARGS[@]}"}"; do
        local key="${kv%%=*}"
        unset "$key" 2>/dev/null || true
    done
}

# ---------- 构建 runtime ----------
build_runtime() {
    local mode="$1"
    if [[ "$SKIP_BUILD" -eq 1 ]]; then
        info "跳过构建 (--skip-build), 使用已有 $BUILD_DYLIB"
        [[ -f "$BUILD_DYLIB" ]] || fail "构建产物不存在: $BUILD_DYLIB"
        return
    fi
    info "构建 runtime (gc-mode=$mode)..."
    cd "$RUNTIME_ROOT"
    "$PYTHON_BIN" build.py build --gc-mode "$mode"
    [[ -f "$BUILD_DYLIB" ]] || fail "构建失败: $BUILD_DYLIB 不存在"
}

# ---------- 替换 SDK dylib ----------
replace_sdk_dylib() {
    info "替换 SDK dylib..."
    cp "$BUILD_DYLIB" "$SDK_DYLIB"
    codesign -f -s - "$SDK_DYLIB"
    info "替换完成并已重签名"
}

# ---------- 恢复 SDK dylib ----------
restore_sdk_dylib() {
    if [[ -f "$SDK_DYLIB_ORIGIN" ]]; then
        info "恢复 SDK 原始 dylib..."
        cp "$SDK_DYLIB_ORIGIN" "$SDK_DYLIB"
        codesign -f -s - "$SDK_DYLIB"
    fi
}

# ---------- 编译用例 ----------
compile_benchmark() {
    local out_dir="$1"
    local binary_name="$2"
    info "Source SDK env: $SDK_ENV"
    export DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}"
    set +u
    # shellcheck disable=SC1090
    source "$SDK_ENV"
    set -u
    export_extra_env

    info "编译 benchmark: $BENCHMARK_SRC"
    (cd "$out_dir" && cjc "$BENCHMARK_SRC" -o "$binary_name")
}

# ---------- 运行 benchmark ----------
run_benchmark() {
    local out_dir="$1"
    local binary="$2"
    local label="$3"

    info "运行 $label benchmark (heap=$HEAP_SIZE, interval=${SAMPLE_INTERVAL_MS}ms)..."

    # source SDK env each time in case env was lost
    export DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}"
    set +u
    # shellcheck disable=SC1090
    source "$SDK_ENV"
    set -u
    export_extra_env

    if [[ -f "$RSS_MONITOR" ]]; then
        # 使用 rss_monitor.py 采集 RSS/CPU
        MRT_REPORT="$out_dir/gc.log" \
        cjHeapSize="$HEAP_SIZE" \
        "$PYTHON_BIN" "$RSS_MONITOR" \
            --binary "$binary" \
            --label "$label" \
            --interval "$SAMPLE_INTERVAL_MS" \
            --vmmap-interval 0 \
            --csv "$out_dir/rss.csv" \
            --output "$out_dir/rss.png" \
            --event-csv "$out_dir/bench_events.csv" \
            --stdout-file "$out_dir/stdout.txt" \
            > "$out_dir/monitor.txt"
    else
        # 回退：简单执行 + 手动采集 RSS
        info "rss_monitor.py 不存在，使用简单模式运行"
        MRT_REPORT="$out_dir/gc.log" \
        cjHeapSize="$HEAP_SIZE" \
        "$binary" > "$out_dir/stdout.txt" 2>&1
    fi

    info "$label benchmark 完成"
}

# ---------- 生成分析报告 ----------
run_analysis() {
    local out_dir="$1"
    if [[ -f "$ANALYZER" ]]; then
        info "生成分析报告..."
        "$PYTHON_BIN" "$ANALYZER" --results-dir "$out_dir" || info "分析脚本报错 (非致命)"
    fi
}

# ---------- 创建结果目录 ----------
make_result_dir() {
    local suffix="$1"
    local ts
    ts="$(date +%Y%m%d_%H%M%S)"
    local dir_name="${ts}_${suffix}"
    [[ -n "$RUN_TAG" ]] && dir_name="${ts}_${RUN_TAG}_${suffix}"
    local dir="$CASE_DIR/result/$dir_name"
    mkdir -p "$dir"
    printf '%s\n' "$dir"
}

# ---------- 保存元数据 ----------
save_metadata() {
    local out_dir="$1"
    local mode="$2"
    {
        echo "case=$CASE_NAME"
        echo "gc_mode=$mode"
        echo "heap_size=$HEAP_SIZE"
        echo "sample_interval_ms=$SAMPLE_INTERVAL_MS"
        echo "tag=$RUN_TAG"
        echo "seed=$SEED_NAME"
        echo "benchmark_src=$BENCHMARK_SRC"
        echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        for kv in "${SEED_ENV_ARGS[@]+"${SEED_ENV_ARGS[@]}"}"; do
            echo "seed_env=$kv"
        done
        for kv in "${EXTRA_ENV_ARGS[@]+"${EXTRA_ENV_ARGS[@]}"}"; do
            echo "env=$kv"
        done
    } > "$out_dir/run_metadata.env"

    # 保存源码快照
    cp "$BENCHMARK_SRC" "$out_dir/benchmark.cj.snapshot"
}

# ---------- 单模式执行流程 ----------
run_single_mode() {
    local mode="$1"  # copygc | marksweep
    local label
    [[ "$mode" == "copygc" ]] && label="CopyGC" || label="MSGC"

    local result_dir
    result_dir="$(make_result_dir "$mode")"
    info "结果目录: $result_dir"

    # marksweep 模式加载 seed 环境变量
    if [[ "$mode" == "marksweep" ]]; then
        export_seed_and_extra_env
    else
        export_extra_env
    fi

    save_metadata "$result_dir" "$mode"
    build_runtime "$mode"
    replace_sdk_dylib
    compile_benchmark "$result_dir" "benchmark"
    run_benchmark "$result_dir" "$result_dir/benchmark" "$label"
    run_analysis "$result_dir"

    [[ "$mode" == "marksweep" ]] && unset_seed_env

    info "===== $label 完成: $result_dir ====="
}

# ---------- 对比模式执行流程 ----------
run_compare_mode() {
    local ts
    ts="$(date +%Y%m%d_%H%M%S)"
    local dir_name="${ts}_compare"
    [[ -n "$RUN_TAG" ]] && dir_name="${ts}_${RUN_TAG}_compare"
    local compare_dir="$CASE_DIR/result/$dir_name"
    local copygc_dir="$compare_dir/copygc"
    local msgc_dir="$compare_dir/msgc"
    mkdir -p "$copygc_dir" "$msgc_dir"

    save_metadata "$compare_dir" "compare"

    # ---- CopyGC ----
    info "========== CopyGC Phase =========="
    save_metadata "$copygc_dir" "copygc"
    build_runtime copygc
    replace_sdk_dylib
    compile_benchmark "$copygc_dir" "benchmark"
    cp "$BENCHMARK_SRC" "$copygc_dir/benchmark.cj.snapshot"
    run_benchmark "$copygc_dir" "$copygc_dir/benchmark" "CopyGC"
    run_analysis "$copygc_dir"

    # ---- MSGC ----
    info "========== MSGC Phase =========="
    export_seed_and_extra_env
    save_metadata "$msgc_dir" "marksweep"
    build_runtime marksweep
    replace_sdk_dylib
    compile_benchmark "$msgc_dir" "benchmark"
    cp "$BENCHMARK_SRC" "$msgc_dir/benchmark.cj.snapshot"
    run_benchmark "$msgc_dir" "$msgc_dir/benchmark" "MSGC"
    run_analysis "$msgc_dir"
    unset_seed_env

    # ---- 对比报告 ----
    if [[ -f "$SCRIPT_DIR/compare_results.py" ]]; then
        info "生成对比报告..."
        "$PYTHON_BIN" "$SCRIPT_DIR/compare_results.py" \
            --left "$copygc_dir" \
            --right "$msgc_dir" \
            --left-label CopyGC \
            --right-label MSGC \
            --output "$compare_dir/compare_report.md" \
            || info "对比脚本报错 (非致命)"
    fi

    # ---- 恢复 CopyGC ----
    restore_sdk_dylib

    info "===== 对比完成: $compare_dir ====="
}

# ---------- MAIN ----------
load_seed
cd "$RUNTIME_ROOT"

case "$GC_MODE" in
    copygc)    run_single_mode copygc ;;
    marksweep) run_single_mode marksweep ;;
    compare)   run_compare_mode ;;
    *)         fail "无效的 gc-mode: $GC_MODE (可选: copygc | marksweep | compare)" ;;
esac
