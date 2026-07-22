#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CENTRAL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"

extract_function() {
    local file="$1"
    local needle="$2"
    awk -v needle="$needle" '
        !capture && index($0, needle) { capture = 1 }
        capture {
            print
            line = $0
            opens = gsub(/\{/, "{", line)
            closes = gsub(/\}/, "}", line)
            depth += opens - closes
            if (opens > 0) { saw_open = 1 }
            if (saw_open && depth == 0) { exit }
        }
    ' "$file"
}

background_order_body="$(extract_function "$CENTRAL_SOURCE" \
    'CentralRegionPool::BuildBackgroundSweepClassOrder')"
background_body="$(extract_function "$CENTRAL_SOURCE" \
    'void CentralRegionPool::BackgroundSweep')"
helper_body="$(extract_function "$CENTRAL_SOURCE" \
    'void CentralRegionPool::ParallelSweep')"
assist_body="$(extract_function "$CENTRAL_SOURCE" \
    'size_t CentralRegionPool::AssistSweep')"

if [[ -z "$background_order_body" || -z "$background_body" ||
      -z "$helper_body" || -z "$assist_body" ]]; then
    echo "failed to extract Sweep class-order functions" >&2
    exit 1
fi

if ! printf '%s\n' "$background_order_body" | rg -q 'GetBucketPressure' ||
   ! printf '%s\n' "$background_order_body" | rg -q 'BuildMSSweepPressureClassOrder'; then
    echo "Background Sweep must snapshot bucket pressure and sort the immutable snapshot" >&2
    exit 1
fi

if ! printf '%s\n' "$background_body" | rg -q 'BuildBackgroundSweepClassOrder' ||
   printf '%s\n' "$background_body" | rg -q 'BuildMSSweepCyclicClassOrder'; then
    echo "Background Sweep must build one pressure order per slice" >&2
    exit 1
fi

if ! printf '%s\n' "$helper_body" | rg -q 'BuildMSSweepCyclicClassOrder' ||
   printf '%s\n' "$helper_body" | rg -q 'BuildBackgroundSweepClassOrder'; then
    echo "Helper Sweep must use its round-robin cyclic order without pressure sorting" >&2
    exit 1
fi

if ! printf '%s\n' "$assist_body" | rg -q 'BuildMSSweepCyclicClassOrder' ||
   printf '%s\n' "$assist_body" | rg -q 'BuildBackgroundSweepClassOrder'; then
    echo "Assist Sweep must start at preferredCls and cyclically fall back without pressure sorting" >&2
    exit 1
fi

background_order_mentions="$(rg -c 'BuildBackgroundSweepClassOrder' "$CENTRAL_SOURCE")"
if [[ "$background_order_mentions" -ne 2 ]]; then
    echo "only the Background order definition and Background slice call may mention pressure sorting" >&2
    exit 1
fi

echo "MSGC Sweep class-order contract holds"
