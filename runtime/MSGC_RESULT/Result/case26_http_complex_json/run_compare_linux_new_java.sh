#!/usr/bin/env bash
# case26 对比脚本入口：使用 java_new_benchmark/Case27Benchmark.java。
# 其余 CopyGC、MSGC、Go、采样和结果汇总逻辑复用 run_compare_linux.sh。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export COPYGC_BENCHMARK_SOURCE="${COPYGC_BENCHMARK_SOURCE:-$SCRIPT_DIR/cj_copygc_benchmark/benchmark.cj}"
export MSGC_BENCHMARK_SOURCE="${MSGC_BENCHMARK_SOURCE:-$SCRIPT_DIR/cj_msgc_benchmark/benchmark.cj}"
export JAVA_BENCHMARK_SOURCE="$SCRIPT_DIR/java_new_benchmark/Case27Benchmark.java"
export JAVA_BENCHMARK_TEST_SOURCE=""
export JAVA_BENCHMARK_MAIN="Case27Benchmark"
export JAVA_BENCHMARK_TEST_MAIN=""
export JAVA_BENCHMARK_CLASSPATH="$SCRIPT_DIR/java_new_benchmark/lib/*"

exec "$SCRIPT_DIR/run_compare_linux.sh" "$@"
