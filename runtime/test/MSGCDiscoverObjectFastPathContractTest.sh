#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR_CPP="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
BARRIER_CPP="$ROOT_DIR/src/Heap/MBarrier/MSMarkBarrier.cpp"

extract_function() {
    local file="$1"
    local signature="$2"
    awk -v signature="$signature" '
        index($0, signature) == 1 { capture = 1 }
        capture {
            print
            line = $0
            opens += gsub(/\{/, "", line)
            line = $0
            closes += gsub(/\}/, "", line)
            if (opens > 0 && opens == closes) {
                exit
            }
        }
    ' "$file"
}

require_present() {
    local text="$1"
    local pattern="$2"
    local message="$3"
    if ! grep -Eq "$pattern" <<<"$text"; then
        echo "$message" >&2
        exit 1
    fi
}

require_absent() {
    local text="$1"
    local pattern="$2"
    local message="$3"
    if grep -Eq "$pattern" <<<"$text"; then
        echo "$message" >&2
        exit 1
    fi
}

require_order() {
    local text="$1"
    shift
    local previous=0
    local pattern
    for pattern in "$@"; do
        local current
        current="$(grep -n -m1 -E "$pattern" <<<"$text" | cut -d: -f1 || true)"
        if [[ -z "$current" || "$current" -le "$previous" ]]; then
            echo "required DiscoverObject order is missing at: $pattern" >&2
            exit 1
        fi
        previous="$current"
    done
}

discover="$(extract_function "$COLLECTOR_CPP" \
    'bool MarkSweepCollector::DiscoverObject(BaseObject* obj, TypeInfo** markedTypeInfo)')"
resolve="$(extract_function "$COLLECTOR_CPP" \
    'static inline bool ResolveMarkTargetImpl(BaseObject* obj, ResolvedMarkTarget& out,')"
trace="$(extract_function "$COLLECTOR_CPP" \
    'void MarkSweepCollector::TraceObjectRefFieldsToGCWork(BaseObject* obj, TypeInfo* typeInfo,')"
enum_ref="$(extract_function "$COLLECTOR_CPP" 'void MarkSweepCollector::EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const')"
enum_raw="$(extract_function "$COLLECTOR_CPP" 'void MarkSweepCollector::EnumAndTagRawRoot(ObjectRef& root, RootSet& rootSet) const')"
enqueue_fast="$(extract_function "$BARRIER_CPP" 'static inline void EnqueueRefFast(BaseObject* ref)')"
enqueue_inner="$(extract_function "$BARRIER_CPP" 'static inline void EnqueueRefInto(MarkSweepCollector* collector,')"

require_present "$discover" 'ResolveMarkTarget\(' \
    'DiscoverObject must resolve the mark target exactly once'
require_order "$discover" 'ResolveMarkTarget\(' '\.IsMarked\(' 'GetTypeInfo\(' '\.TryMark\('
require_absent "$discover" 'IsValidObject\(' \
    'DiscoverObject must reuse the TypeInfo acquired for white-slot validation'
require_absent "$discover" 'allocBitmap' \
    'DiscoverObject fast path must not read the concurrently mutated allocBitmap'
require_present "$resolve" 'GetRegionInfoDirect\(' \
    'ResolveMarkTargetImpl must derive RegionInfo and Region offset in one direct lookup'
require_absent "$resolve" 'MSRegionInfo\* info = MSRegionInfoArray::GetRegionInfoValidated\(' \
    'ResolveMarkTargetImpl Release lookup must not start from adjacent-entry validation'

require_absent "$trace" 'IsManagedAddress|IsHeapAddress' \
    'object graph tracing must leave heap validation to DiscoverObject'
require_present "$trace" 'ForEachRefFieldGCKnownType' \
    'object graph tracing contract must inspect the current known-TypeInfo scanner'
require_absent "$enum_ref" 'IsManagedAddress|IsHeapAddress' \
    'reference-root enumeration must leave heap validation to DiscoverObject'
require_absent "$enum_raw" 'IsManagedAddress|IsHeapAddress' \
    'raw-root enumeration must leave heap validation to DiscoverObject'

require_present "$enqueue_fast" 'Heap::IsHeapAddress' \
    'barrier outer fast path must reject non-heap refs before resolving TLS owner'
require_absent "$enqueue_inner" 'Heap::IsHeapAddress' \
    'barrier inner path must trust the outer heap gate instead of checking again'

echo 'MSGC DiscoverObject fast-path contract holds'
