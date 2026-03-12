#!/bin/bash
# ============================================================
# GC Benchmark 运行 + 基线采集脚本
# 用法:
#   ./run_benchmark.sh              # 编译 + 运行 + 分析
#   ./run_benchmark.sh compile      # 仅编译
#   ./run_benchmark.sh run          # 仅运行（需已编译）
#   ./run_benchmark.sh analyze      # 仅分析日志（需已运行）
# ============================================================

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_BASE="/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
err()   { echo -e "${RED}[ERROR]${NC} $*"; }

BENCH_BIN="$SCRIPT_DIR/gc_benchmark"
BENCH_SRC="$SCRIPT_DIR/gc_benchmark.cj"
STDOUT_FILE="$SCRIPT_DIR/baseline_stdout.txt"
GC_LOG="$SCRIPT_DIR/baseline_gc.log"
BASELINE_JSON="$SCRIPT_DIR/baseline_benchmark.json"

do_compile() {
    info "设置 SDK 环境 ..."
    source "$SDK_BASE/envsetup.sh"

    info "编译 gc_benchmark.cj ..."
    cd "$SCRIPT_DIR"
    cjc "$BENCH_SRC" -o gc_benchmark
    ok "编译完成: $BENCH_BIN"
}

do_run() {
    info "设置 SDK 环境 ..."
    source "$SDK_BASE/envsetup.sh"

    if [[ ! -f "$BENCH_BIN" ]]; then
        err "gc_benchmark 不存在，请先运行: $0 compile"
        exit 1
    fi

    info "运行 GC Benchmark (heap=256mb, 日志→baseline_gc.log) ..."
    cd "$SCRIPT_DIR"
    MRT_REPORT="$GC_LOG" cjHeapSize=256mb "$BENCH_BIN" > "$STDOUT_FILE" 2>&1 || true

    ok "运行完成"
    echo ""
    cat "$STDOUT_FILE"
    echo ""

    if [[ -f "$GC_LOG" ]]; then
        local lines
        lines=$(wc -l < "$GC_LOG")
        ok "GC 日志: $GC_LOG ($lines 行)"
    else
        err "GC 日志未生成"
    fi
}

do_analyze() {
    if [[ ! -f "$STDOUT_FILE" ]]; then
        err "stdout 文件不存在: $STDOUT_FILE"
        exit 1
    fi

    info "提取 benchmark 结果 ..."
    echo ""
    echo "=== [BENCH] 结果摘要 ==="
    grep "\\[BENCH\\]" "$STDOUT_FILE"
    echo ""

    if [[ -f "$GC_LOG" ]]; then
        info "分析 GC 日志 ..."
        python3 "$SCRIPT_DIR/analyze_benchmark.py" \
            --stdout "$STDOUT_FILE" \
            --gclog "$GC_LOG" \
            --output "$BASELINE_JSON"
        echo ""
        ok "基线 JSON 已保存: $BASELINE_JSON"
    else
        info "无 GC 日志，仅提取 stdout 数据 ..."
        python3 "$SCRIPT_DIR/analyze_benchmark.py" \
            --stdout "$STDOUT_FILE" \
            --output "$BASELINE_JSON"
        echo ""
        ok "基线 JSON 已保存: $BASELINE_JSON"
    fi
}

CMD="${1:-all}"
case "$CMD" in
    compile)  do_compile ;;
    run)      do_run ;;
    analyze)  do_analyze ;;
    all)
        do_compile
        do_run
        do_analyze
        ;;
    *)
        echo "用法: $0 {compile|run|analyze|all}"
        exit 1
        ;;
esac
