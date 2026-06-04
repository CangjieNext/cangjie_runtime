#!/usr/bin/env bash
# ============================================================
# Case25 对比：dev260602(MSGC,d509 stdx overlay) vs dev/20260507(CopyGC)
# 两端各自用自带 SDK 编译并跑 case25 benchmark；不重新 build runtime，不替换 dylib。
# 单次对比只跑一组 (CASE25_PAYLOAD × CASE25_CONCURRENCY)。
#
#   Env vars (optional, with defaults):
#     CASE25_PAYLOAD       1K|10K|100K|1M|10M    default 10K
#     CASE25_CONCURRENCY   worker count           default 8
#     CASE25_DURATION_MS   bench duration         default 60000
#     HEAP_SIZE            cjHeapSize             default 512mb
#     PROCS                cjProcessorNum         default 6
#     TAG                  result dir tag         default d602_vs_d507
# ============================================================
set -uo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNTIME_ROOT="$(cd "$CASE_DIR/../../.." && pwd)"
SCRIPT_DIR="$RUNTIME_ROOT/MSGC_RESULT/Result/script"
PYTHON_BIN="${PYTHON_BIN:-$RUNTIME_ROOT/.venv/bin/python}"
[[ -x "$PYTHON_BIN" ]] || PYTHON_BIN="python3"

MSGC_SDK="/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260602/cangjie"
MSGC_STDX="/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260602/darwin_aarch64_cjnative/dynamic/stdx"
COPYGC_SDK="/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie"
COPYGC_STDX="/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative/dynamic/stdx"

PAYLOAD="${CASE25_PAYLOAD:-10K}"
CONC="${CASE25_CONCURRENCY:-8}"
DUR="${CASE25_DURATION_MS:-60000}"
HEAP_SIZE="${HEAP_SIZE:-512mb}"
PROCS="${PROCS:-6}"
TAG="${TAG:-d602_vs_d507}"

if [[ ! -f "$CASE_DIR/payloads/${PAYLOAD}.json" ]]; then
    echo "[ERROR] payloads/${PAYLOAD}.json not found" >&2
    exit 1
fi

TS="$(date +%Y%m%d_%H%M%S)"
COMPARE_DIR="$CASE_DIR/result/${TS}_${TAG}_c${CONC}_p${PAYLOAD}_compare"
COPYGC_DIR="$COMPARE_DIR/copygc"
MSGC_DIR="$COMPARE_DIR/msgc"
mkdir -p "$COPYGC_DIR" "$MSGC_DIR"
echo "[INFO] compare dir: $COMPARE_DIR"
echo "[INFO] config: payload=$PAYLOAD concurrency=$CONC duration=${DUR}ms heap=$HEAP_SIZE proc=$PROCS"

run_side() {
    local label="$1"        # CopyGC | MSGC
    local out_dir="$2"
    local sdk_root="$3"
    local stdx_dir="$4"
    # use different ports per side to avoid leftover port reuse
    local port="$5"

    echo "[INFO] ===== $label ($sdk_root) ====="
    {
        echo "case=case25_http_complex_json"
        echo "label=$label"
        echo "sdk_root=$sdk_root"
        echo "stdx_dir=$stdx_dir"
        echo "payload=$PAYLOAD"
        echo "concurrency=$CONC"
        echo "duration_ms=$DUR"
        echo "server_port=$port"
        echo "heap_size=$HEAP_SIZE"
        echo "procs=$PROCS"
        echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "runtime_dylib_md5=$(md5 -q $sdk_root/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib)"
    } > "$out_dir/run_metadata.env"
    cp "$CASE_DIR/benchmark.cj" "$out_dir/benchmark.cj.snapshot"
    cp "$CASE_DIR/payloads/${PAYLOAD}.json" "$out_dir/payload.snapshot.json"

    env -i HOME="$HOME" TERM="${TERM:-xterm}" PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin" \
        SDK_ROOT="$sdk_root" STDX_DIR="$stdx_dir" OUT_DIR="$out_dir" LABEL="$label" \
        HEAP_SIZE="$HEAP_SIZE" PROCS="$PROCS" \
        BENCH_SRC="$CASE_DIR/benchmark.cj" \
        PAYLOAD_DIR="$CASE_DIR/payloads" \
        CASE25_PAYLOAD="$PAYLOAD" CASE25_CONCURRENCY="$CONC" \
        CASE25_DURATION_MS="$DUR" CASE25_SERVER_PORT="$port" \
        CASE25_PAYLOAD_DIR="$CASE_DIR/payloads" \
        CJ_MS_DISABLE_ASSIST="${CJ_MS_DISABLE_ASSIST:-}" \
        CJ_MS_ASSIST_RATIO_SCALE="${CJ_MS_ASSIST_RATIO_SCALE:-}" \
        CJ_MS_ASSIST_DEBT_TRIGGER="${CJ_MS_ASSIST_DEBT_TRIGGER:-}" \
        bash -c '
            set -o pipefail
            set +u
            source "$SDK_ROOT/envsetup.sh" >/dev/null 2>&1
            set -u
            export DYLD_LIBRARY_PATH="$STDX_DIR:${DYLD_LIBRARY_PATH:-}"
            echo "[$LABEL] cjc: $(which cjc)"
            echo "[$LABEL] DYLD_LIBRARY_PATH=$DYLD_LIBRARY_PATH"

            cd "$OUT_DIR"
            cjc "$BENCH_SRC" \
                --import-path "$STDX_DIR" \
                -L "$STDX_DIR" \
                --link-option "$STDX_DIR/libstdx.net.http.dylib" \
                --link-option "$STDX_DIR/libstdx.encoding.json.dylib" \
                -o benchmark
            if [ ! -x benchmark ]; then
                echo "[$LABEL] compile failed"; exit 1
            fi

            MRT_REPORT="$OUT_DIR/gc.log" \
            cjHeapSize="$HEAP_SIZE" \
            cjProcessorNum="$PROCS" \
                "$OUT_DIR/benchmark" > "$OUT_DIR/stdout.txt" 2>&1 &
            BENCH_PID=$!

            # 后台采样 RSS/CPU (perl 提供亚秒时间戳)
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

# CopyGC 先跑（基线），用端口 18227 (18225 被历史 UE 进程占用)
run_side "CopyGC" "$COPYGC_DIR" "$COPYGC_SDK" "$COPYGC_STDX" 18227
COPYGC_RC=$?

# MSGC 再跑，用端口 18228 (18226/18227 被历史 UE 进程占用)
run_side "MSGC" "$MSGC_DIR" "$MSGC_SDK" "$MSGC_STDX" 18228
MSGC_RC=$?

echo "[INFO] CopyGC rc=$COPYGC_RC  MSGC rc=$MSGC_RC"

if [ -f "$SCRIPT_DIR/compare_results.py" ]; then
    "$PYTHON_BIN" "$SCRIPT_DIR/compare_results.py" \
        --left "$COPYGC_DIR" \
        --right "$MSGC_DIR" \
        --left-label CopyGC \
        --right-label MSGC \
        --output "$COMPARE_DIR/compare_report.md" \
        || echo "[WARN] compare_results.py failed (non-fatal)"
fi

echo "[DONE] compare_dir=$COMPARE_DIR"
