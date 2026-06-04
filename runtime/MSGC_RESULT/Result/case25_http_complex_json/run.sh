#!/usr/bin/env bash
# ============================================================
#  case25 single-combo runner: compile (if needed) + run + capture
#  output (stdout / GC log / RSS&CPU samples) into result/<ts>_*.
#
#  Env vars (all optional):
#    SDK_ROOT            : Cangjie SDK root (default: d602 MSGC)
#    STDX_DIR            : stdx dylib dir   (default: d602 stdx)
#    CASE25_PAYLOAD      : 1K|10K|100K|1M|10M           (default 10K)
#    CASE25_CONCURRENCY  : worker count                 (default 8)
#    CASE25_DURATION_MS  : run duration in ms           (default 60000)
#    CASE25_SERVER_PORT  : server port                  (default 18125)
#    CASE25_LABEL        : label used in result dir name (default sdk basename)
#    HEAP                : cjHeapSize value             (default 512mb)
#    PROC                : cjProcessorNum value         (default 6)
#    REBUILD             : 1 to force `cjc` recompile   (default 1 if no binary)
#
#  Output dir: result/<YYYYMMDD_HHMMSS>_${LABEL}_c${CONC}_p${PAYLOAD}/
#    benchmark.stdout.txt   - program stdout/stderr
#    gc.log                 - MRT_REPORT GC log
#    rss.csv                - per-200ms RSS(kb) + CPU% samples (headerless ts,rss_kb,cpu_pct)
#    run_meta.txt           - env+config snapshot
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

SDK_ROOT="${SDK_ROOT:-/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260602/cangjie}"
STDX_DIR="${STDX_DIR:-/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260602/darwin_aarch64_cjnative/dynamic/stdx}"
PAYLOAD="${CASE25_PAYLOAD:-10K}"
CONC="${CASE25_CONCURRENCY:-8}"
DUR="${CASE25_DURATION_MS:-60000}"
PORT="${CASE25_SERVER_PORT:-18125}"
HEAP="${HEAP:-512mb}"
PROC="${PROC:-6}"
LABEL="${CASE25_LABEL:-$(basename "$(dirname "$SDK_ROOT")")}"
REBUILD="${REBUILD:-auto}"

if [[ ! -f "$HERE/payloads/${PAYLOAD}.json" ]]; then
    echo "[ERROR] payload not found: payloads/${PAYLOAD}.json" >&2
    exit 1
fi

if [[ "$REBUILD" == "1" || ( "$REBUILD" == "auto" && ! -x "$HERE/benchmark" ) ]]; then
    echo "[BUILD] compiling benchmark.cj with $SDK_ROOT ..."
    env -i HOME="$HOME" TERM="${TERM:-xterm}" PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
        bash -c "
            set +u
            source '$SDK_ROOT/envsetup.sh' >/dev/null 2>&1
            set -u
            cd '$HERE'
            cjc benchmark.cj \
              --import-path '$STDX_DIR' \
              -L '$STDX_DIR' \
              --link-option '$STDX_DIR/libstdx.net.http.dylib' \
              --link-option '$STDX_DIR/libstdx.encoding.json.dylib' \
              -o benchmark
        "
fi

TS="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$HERE/result/${TS}_${LABEL}_c${CONC}_p${PAYLOAD}"
mkdir -p "$OUT_DIR"
STDOUT_FILE="$OUT_DIR/benchmark.stdout.txt"
GC_LOG="$OUT_DIR/gc.log"
RSS_FILE="$OUT_DIR/rss.csv"
META="$OUT_DIR/run_meta.txt"

{
    echo "case=case25_http_complex_json"
    echo "ts=$TS"
    echo "label=$LABEL"
    echo "sdk_root=$SDK_ROOT"
    echo "stdx_dir=$STDX_DIR"
    echo "payload=$PAYLOAD"
    echo "concurrency=$CONC"
    echo "duration_ms=$DUR"
    echo "server_port=$PORT"
    echo "heap=$HEAP"
    echo "processors=$PROC"
    echo "host=$(hostname)"
    echo "os=$(uname -a)"
} > "$META"

echo "[RUN ] writing to $OUT_DIR"
echo "[RUN ] payload=$PAYLOAD concurrency=$CONC duration=${DUR}ms heap=$HEAP proc=$PROC"

# Launch benchmark in background under clean env, then sample RSS/CPU every 0.2s.
env -i HOME="$HOME" TERM="${TERM:-xterm}" PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
    CASE25_PAYLOAD="$PAYLOAD" CASE25_CONCURRENCY="$CONC" CASE25_DURATION_MS="$DUR" CASE25_SERVER_PORT="$PORT" \
    MRT_REPORT="$GC_LOG" cjHeapSize="$HEAP" cjProcessorNum="$PROC" \
    SDK_ENVSETUP="$SDK_ROOT/envsetup.sh" STDX_DIR="$STDX_DIR" BIN_PATH="$HERE/benchmark" \
    bash -c '
        set +u
        source "$SDK_ENVSETUP" >/dev/null 2>&1
        set -u
        export DYLD_LIBRARY_PATH="$STDX_DIR:${DYLD_LIBRARY_PATH:-}"
        exec "$BIN_PATH"
    ' >"$STDOUT_FILE" 2>&1 &
BENCH_PID=$!

# Background RSS/CPU sampler (perl for sub-second wall time on macOS BSD date)
perl -MTime::HiRes=sleep,time -e '
    my $pid = $ARGV[0];
    while (kill 0, $pid) {
        my $line = `ps -o rss=,%cpu= -p $pid 2>/dev/null`;
        chomp $line;
        $line =~ s/^\s+//;
        if (length $line) {
            my ($rss, $cpu) = split /\s+/, $line, 2;
            my $t = time();
            my @lt = localtime(int($t));
            my $frac = sprintf("%.3f", $t - int($t));
            $frac =~ s/^0//;
            printf "%04d-%02d-%02dT%02d:%02d:%02d%s,%s,%s\n",
                $lt[5]+1900,$lt[4]+1,$lt[3],$lt[2],$lt[1],$lt[0],$frac,$rss,$cpu;
        }
        sleep(0.2);
    }
' "$BENCH_PID" > "$RSS_FILE" &
SAMPLER_PID=$!

wait "$BENCH_PID" || true
kill "$SAMPLER_PID" 2>/dev/null || true
wait "$SAMPLER_PID" 2>/dev/null || true

echo "[DONE] result -> $OUT_DIR"
tail -10 "$STDOUT_FILE" | sed 's/^/    /'
