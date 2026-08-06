#!/usr/bin/env bash
set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RERUNS="${RERUNS:-3}"
TAG_PREFIX="${TAG_PREFIX:-enum_assist_verify}"

required_vars=(
  MSGC_SDK_ROOT
  MSGC_STDX_ROOT
  COPYGC_SDK_ROOT
  COPYGC_STDX_ROOT
)

for var_name in "${required_vars[@]}"; do
  if [[ -z "${!var_name:-}" ]]; then
    echo "[ERROR] $var_name is not set" >&2
    exit 1
  fi
done

run_one() {
  local index="$1"
  local tag="${TAG_PREFIX}_run${index}"
  local run_log
  run_log="$(mktemp -t case27_enum_assist.XXXXXX)"

  echo "[INFO] rerun ${index}/${RERUNS} tag=${tag}"

  (
    cd "$CASE_DIR"
    TAG="$tag" ./run_compare.sh
  ) | tee "$run_log"

  local compare_dir
  compare_dir="$(sed -n 's/^\[INFO\] compare dir: //p' "$run_log" | tail -n 1)"
  rm -f "$run_log"

  if [[ -z "$compare_dir" || ! -d "$compare_dir" ]]; then
    echo "[ERROR] compare dir not found for rerun $index" >&2
    exit 1
  fi

  local msgc_stdout="$compare_dir/msgc/stdout.txt"
  local msgc_gc_log="$compare_dir/msgc/gc.log"

  if [[ ! -s "$msgc_stdout" ]]; then
    echo "[ERROR] missing msgc stdout: $msgc_stdout" >&2
    exit 1
  fi
  if [[ ! -s "$msgc_gc_log" ]]; then
    echo "[ERROR] missing msgc gc.log: $msgc_gc_log" >&2
    exit 1
  fi

  if rg -n "gc transition mutator .* failed|Abort trap: 6|Segmentation fault: 11|catched unhandled SIGABRT|catched unhandled SIGSEGV|CJNatvie Handle signal: 6|CJNatvie Handle signal: 11|Handle unexpected signal action" \
      "$msgc_stdout" >/dev/null; then
    echo "[ERROR] runtime crash detected in $compare_dir" >&2
    exit 1
  fi

  if rg -n " gc transition mutator " "$msgc_gc_log" >/dev/null; then
    echo "[ERROR] transition failure marker detected in gc.log: $compare_dir" >&2
    exit 1
  fi

  if ! rg -n "^\[RESULT\]|^\[DONE\]$" "$msgc_stdout" >/dev/null; then
    echo "[ERROR] missing successful completion markers in $msgc_stdout" >&2
    exit 1
  fi

  echo "[INFO] rerun ${index} passed: $compare_dir"
}

for ((i = 1; i <= RERUNS; ++i)); do
  run_one "$i"
done

echo "[INFO] all ${RERUNS} reruns passed"
