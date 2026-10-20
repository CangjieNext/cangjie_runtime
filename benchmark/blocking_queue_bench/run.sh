#!/usr/bin/env bash
# Run the LinkedBlockingQueue benchmark (after build.sh).
set -euo pipefail
cd "$(dirname "$0")"
if [ -x ./lbq_bench ]; then
  ./lbq_bench
elif [ -f ./lbq_bench.exe ]; then
  ./lbq_bench.exe
else
  echo "lbq_bench not built; run build.sh first" >&2
  exit 1
fi
