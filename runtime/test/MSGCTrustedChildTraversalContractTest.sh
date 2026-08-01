#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

source_text="$(<"$COLLECTOR")"

require_present() {
    local text="$1"
    local pattern="$2"
    local message="$3"
    if ! rg -Uq "$pattern" <<<"$text"; then
        echo "$message" >&2
        exit 1
    fi
}

require_absent() {
    local text="$1"
    local pattern="$2"
    local message="$3"
    if rg -Uq "$pattern" <<<"$text"; then
        echo "$message" >&2
        exit 1
    fi
}

unified_add="$(sed -n \
    '/void MarkSweepCollector::AddRefToMarkBatch(/,/^}/p' \
    "$COLLECTOR")"
require_present "$unified_add" 'Heap::IsHeapAddress\(obj\)' \
    'precise child traversal must retain one heap-address gate'
require_present "$unified_add" \
    'MSMarkRefSource::TRUSTED_MAYBE_MANAGED' \
    'precise child traversal must enter trusted ref-to-bitmap resolution'
require_absent "$unified_add" \
    'allocBitmap|GetAllocBitmap|collectEpoch' \
    'precise child traversal must not revalidate TypeInfo or allocation state'

trusted_publication="$(sed -n \
    '/ValidatePublishedMarkTarget(BaseObject\* obj, MarkSourceTag<true>)/,/^}/p' \
    "$COLLECTOR")"
require_present "$trusted_publication" 'MRT_DEBUG' \
    'trusted publication validation must retain a debug-only assertion'
require_present "$trusted_publication" 'GetTypeInfo\(\)' \
    'debug builds must still diagnose unpublished trusted objects'
trusted_release="$(sed -n \
    '/ValidatePublishedMarkTarget(BaseObject\* obj, MarkSourceTag<true>)/,/^}/p' \
    "$COLLECTOR" | sed -n '/#else/,/#endif/p')"
require_absent "$trusted_release" 'GetTypeInfo\(' \
    'release trusted publication validation must not read TypeInfo'
require_present "$source_text" \
    'ValidatePublishedMarkTarget\([[:space:]]*obj, MarkSourceTag<Trusted> \{\}\)' \
    'Small/Large shared marking must select publication validation by source trust'

precise_call_count="$(rg -c 'AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MAYBE_MANAGED>' "$COLLECTOR")"
if (( precise_call_count < 2 )); then
    echo 'Small Region and Large Object task visitors must both use the precise child path' >&2
    exit 1
fi

require_present "$source_text" \
    'AddRefToMarkBatch<MSMarkRefSource::DEFENSIVE>' \
    'untrusted Runtime sources must retain the defensive path'
require_absent "$source_text" \
    'AddTrustedRefToMarkBatchImpl|AddDefensiveRefToMarkBatchImpl|AddPreciseChildRefToMarkBatch' \
    'source-specific Add wrappers must be replaced by the shared source-policy entry'

echo 'MSGC trusted child traversal contract holds'
