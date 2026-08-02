#!/usr/bin/env bash
set -euo pipefail

runtime_root="$(cd "$(dirname "$0")/.." && pwd)"
header="$runtime_root/src/Heap/MBarrier/MSMarkBarrier.h"
stats_source="$runtime_root/src/Heap/MBarrier/MSMarkBarrier.cpp"
collector_source="$runtime_root/src/Heap/MCollector/MarkSweepCollector.cpp"

retained_tokens=(
    "OLD_REF_RESOLVE_REJECTED"
    "OLD_REF_THRESHOLD_FLUSH"
    "OLD_REF_SET_CONFLICT_FLUSH"
    "old_ref_resolve_rejected"
    "old_ref_threshold_flush"
    "old_ref_set_conflict_flush"
)

for token in "${retained_tokens[@]}"; do
    if ! rg -Fq "$token" "$header" "$stats_source" "$collector_source"; then
        echo "missing rare-event barrier counter token: $token" >&2
        exit 1
    fi
done

removed_tokens=(
    "OLD_REF_BUFFER_REQUEST"
    "OLD_REF_LARGE"
    "OLD_REF_ALREADY_PENDING"
    "OLD_REF_ADDED_WITHOUT_FLUSH"
    "old_ref_buffer_request"
    "old_ref_large"
    "old_ref_already_pending"
    "old_ref_added_without_flush"
)

for token in "${removed_tokens[@]}"; do
    if rg -Fq "$token" "$header" "$stats_source" "$collector_source"; then
        echo "normal-path barrier counter remains: $token" >&2
        exit 1
    fi
done

echo "MSGC rare-event-only barrier counter contract passed"
