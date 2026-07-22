#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MARK_SWEEP_CPP="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

if [[ ! -f "$MARK_SWEEP_CPP" ]]; then
    echo "MarkSweepCollector.cpp not found: $MARK_SWEEP_CPP" >&2
    exit 2
fi

extract_function() {
    local signature="$1"
    awk -v signature="$signature" '
        $0 == signature { capture = 1 }
        capture {
            print
            if ($0 == "}") {
                exit
            }
        }
    ' "$MARK_SWEEP_CPP"
}

require_absent() {
    local haystack="$1"
    local pattern="$2"
    local message="$3"
    if printf '%s\n' "$haystack" | rg -q "$pattern"; then
        echo "$message" >&2
        printf '%s\n' "$haystack" >&2
        exit 1
    fi
}

require_present() {
    local haystack="$1"
    local pattern="$2"
    local message="$3"
    if ! printf '%s\n' "$haystack" | rg -q "$pattern"; then
        echo "$message" >&2
        printf '%s\n' "$haystack" >&2
        exit 1
    fi
}

pre_hook="$(extract_function 'void MarkSweepCollector::PreGarbageCollection(bool isConcurrent)')"
post_hook="$(extract_function 'void MarkSweepCollector::PostGarbageCollection(uint64_t gcIndex)')"

if [[ -z "$pre_hook" || -z "$post_hook" ]]; then
    echo "failed to extract MarkSweepCollector GC hook bodies" >&2
    exit 1
fi

require_absent \
    "$pre_hook" \
    'TracingCollector::PreGarbageCollection|SatbBuffer::Instance\(\)\.Init' \
    "MarkSweepCollector::PreGarbageCollection must not reuse SATB initialization from TracingCollector"

require_absent \
    "$post_hook" \
    'TracingCollector::PostGarbageCollection|SatbBuffer::Instance\(\)\.ReclaimALLPages|GC_PHASE_RECLAIM_SATB_NODE' \
    "MarkSweepCollector::PostGarbageCollection must not reuse SATB reclaim/RECLAIM_SATB_NODE cleanup"

require_absent \
    "$post_hook" \
    'TransitionToGCPhase\(' \
    "MarkSweepCollector::PostGarbageCollection must not repeat MarkTermination's IDLE transition"

require_present \
    "$post_hook" \
    'collectorResources\.NotifyGCFinished\(gcIndex\)' \
    "MarkSweepCollector::PostGarbageCollection must notify collector resources when GC finishes"

echo "MSGC SATB hook decouple contract holds"
