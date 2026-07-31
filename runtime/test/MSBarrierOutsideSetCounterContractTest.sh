#!/usr/bin/env bash
set -euo pipefail

runtime_root="$(cd "$(dirname "$0")/.." && pwd)"
header="$runtime_root/src/Heap/MBarrier/MSMarkBarrier.h"
stats_source="$runtime_root/src/Heap/MBarrier/MSMarkBarrier.cpp"
collector_source="$runtime_root/src/Heap/MCollector/MarkSweepCollector.cpp"

required_tokens=(
    "OLD_REF_ADDED_WITHOUT_FLUSH_COLLECT_SET"
    "OLD_REF_ADDED_WITHOUT_FLUSH_OUTSIDE_SET"
    "old_ref_added_without_flush_collect_set"
    "old_ref_added_without_flush_outside_set"
)

for token in "${required_tokens[@]}"; do
    if ! rg -Fq "$token" "$header" "$stats_source" "$collector_source"; then
        echo "missing outside-set barrier counter token: $token" >&2
        exit 1
    fi
done

if ! rg -Uq \
    'OLD_REF_ADDED_WITHOUT_FLUSH_COLLECT_SET[[:space:]]*:[[:space:]]*MSBarrierRuntimeStat::OLD_REF_ADDED_WITHOUT_FLUSH_OUTSIDE_SET' \
    "$collector_source"; then
    echo "successful buffer add is not split by inCollectSet" >&2
    exit 1
fi

echo "MSGC outside-set barrier counter contract passed"
