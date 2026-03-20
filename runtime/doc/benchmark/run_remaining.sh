#!/usr/bin/env bash
set -euo pipefail

RESDIR="/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/doc/benchmark/results/20260319_232218"
BINARY="$RESDIR/gc_benchmark_ms2"
SAMPLE_INTERVAL=0.2

run_once() {
    local heap_mb="$1"
    local prefix="$2"
    local stdout_file="$RESDIR/${prefix}_stdout.txt"
    local resource_file="$RESDIR/${prefix}_resource.csv"

    echo "[RUN] $prefix  heap=${heap_mb}mb"

    echo "timestamp,cpu_pct,rss_kb" > "$resource_file"

    cjHeapSize="${heap_mb}mb" "$BINARY" > "$stdout_file" 2>&1 &
    local pid=$!

    while kill -0 "$pid" 2>/dev/null; do
        local ts cpu rss
        ts=$(date +%s)
        read -r cpu rss < <(ps -p "$pid" -o %cpu=,rss= 2>/dev/null || echo "0 0")
        if [[ -n "$cpu" && -n "$rss" ]]; then
            echo "$ts,$cpu,$rss" >> "$resource_file"
        fi
        sleep "$SAMPLE_INTERVAL"
    done

    wait "$pid" || true

    grep '\[BENCH\] Total wall time' "$stdout_file" || echo "  [WARN] No Total found!"
}

echo "Starting remaining MS-GC tests..."
echo "Binary: $BINARY"
echo ""

run_once 256 "ms_256m_r2"
run_once 256 "ms_256m_r3"
run_once 512 "ms_512m_r1"
run_once 512 "ms_512m_r2"
run_once 512 "ms_512m_r3"

echo ""
echo "Done! All remaining tests completed."
