#!/usr/bin/env bash
# ============================================================
#  gc_bench_compare.sh — MS-GC vs CopyGC 自动化对比测试
#
#  用法: bash gc_bench_compare.sh [--rounds N] [--heaps "256 512"]
#
#  执行流程:
#    1. CopyGC: restore → compile → run 3×256M + 3×512M
#    2. MS-GC:  build → replace → compile → run 3×256M + 3×512M
#    3. restore CopyGC
#
#  输出: doc/benchmark/results/<timestamp>/ 下的原始数据
# ============================================================
set -euo pipefail

# ── 配置 ──────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNTIME_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SDK="${SDK:-/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie}"
DYLIB_DIR="$SDK/runtime/lib/darwin_aarch64_cjnative"
DYLIB="$DYLIB_DIR/libcangjie-runtime.dylib"
DYLIB_ORIGIN="$DYLIB.origin"
BENCHMARK_SRC="$SCRIPT_DIR/gc_benchmark.cj"
ROUNDS=3
HEAPS=(256 512)
SAMPLE_INTERVAL=0.2

# ── 参数解析 ──────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rounds) ROUNDS="$2"; shift 2 ;;
        --heaps) IFS=' ' read -r -a HEAPS <<< "$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── 输出目录 ──────────────────────────────────────────────
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_DIR="$SCRIPT_DIR/results/$TIMESTAMP"
mkdir -p "$RESULTS_DIR"

echo "========================================"
echo "  GC Benchmark Compare"
echo "  Rounds: $ROUNDS  Heaps: ${HEAPS[*]}MB"
echo "  Results: $RESULTS_DIR"
echo "========================================"

# ── 辅助函数 ──────────────────────────────────────────────

replace_dylib_ms() {
    echo "[STEP] Building MS-GC runtime..."
    (cd "$RUNTIME_ROOT" && python3 build.py build --gc-mode marksweep)
    echo "[STEP] Replacing SDK dylib with MS-GC..."
    cp "$RUNTIME_ROOT/output/temp/lib/aarch64_Release/libcangjie-runtime.dylib" "$DYLIB"
    codesign -f -s - "$DYLIB"
}

restore_dylib_copy() {
    echo "[STEP] Restoring CopyGC dylib..."
    cp "$DYLIB_ORIGIN" "$DYLIB"
    codesign -f -s - "$DYLIB"
}

compile_benchmark() {
    local binary_name="$1"
    echo "[STEP] Compiling benchmark → $binary_name"
    # envsetup.sh may reference unbound vars (e.g. DYLD_LIBRARY_PATH)
    set +u
    # shellcheck source=/dev/null
    source "$SDK/envsetup.sh"
    set -u
    (cd "$RESULTS_DIR" && cjc "$BENCHMARK_SRC" -o "$binary_name")
}

# 运行一次 benchmark 并采集资源
# 参数: $1=binary_path $2=heap_mb $3=output_prefix
run_once() {
    local binary="$1"
    local heap_mb="$2"
    local prefix="$3"
    local stdout_file="$RESULTS_DIR/${prefix}_stdout.txt"
    local resource_file="$RESULTS_DIR/${prefix}_resource.csv"

    echo "[RUN] $prefix  heap=${heap_mb}mb"

    # 写 CSV 表头
    echo "timestamp,cpu_pct,rss_kb" > "$resource_file"

    # 启动 benchmark 进程
    cjHeapSize="${heap_mb}mb" "$binary" > "$stdout_file" 2>&1 &
    local pid=$!

    # 后台资源采样
    while kill -0 "$pid" 2>/dev/null; do
        local ts cpu rss
        ts=$(date +%s.%N 2>/dev/null || date +%s)
        # ps 输出: %CPU  RSS
        read -r cpu rss < <(ps -p "$pid" -o %cpu=,rss= 2>/dev/null || echo "0 0")
        if [[ -n "$cpu" && -n "$rss" ]]; then
            echo "$ts,$cpu,$rss" >> "$resource_file"
        fi
        sleep "$SAMPLE_INTERVAL"
    done

    wait "$pid" || true

    # 简要打印结果
    grep '^\[BENCH\]' "$stdout_file" || true
    echo ""
}

# 批量运行某 GC 模式
# 参数: $1=gc_label (copy/ms) $2=binary_name
run_all_heaps() {
    local gc_label="$1"
    local binary="$RESULTS_DIR/$2"

    for heap in "${HEAPS[@]}"; do
        for r in $(seq 1 "$ROUNDS"); do
            local prefix="${gc_label}_${heap}m_r${r}"
            run_once "$binary" "$heap" "$prefix"
        done
    done
}

# ── 主流程 ────────────────────────────────────────────────

echo ""
echo "======== Phase 1: CopyGC ========"
restore_dylib_copy
compile_benchmark "gc_benchmark_copy"
run_all_heaps "copy" "gc_benchmark_copy"

echo ""
echo "======== Phase 2: MS-GC ========"
replace_dylib_ms
compile_benchmark "gc_benchmark_ms"
run_all_heaps "ms" "gc_benchmark_ms"

echo ""
echo "======== Cleanup: Restore CopyGC ========"
restore_dylib_copy

echo ""
echo "========================================"
echo "  Done! Raw results in: $RESULTS_DIR"
echo "  Run:  python3 gc_bench_analyze.py $RESULTS_DIR"
echo "========================================"
