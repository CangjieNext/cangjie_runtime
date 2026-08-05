#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
BARRIER="$ROOT_DIR/src/Heap/MBarrier/MSMarkBarrier.cpp"

source_text="$(<"$COLLECTOR")"

require_present() {
    local pattern="$1"
    local message="$2"
    if ! rg -Uq "$pattern" <<<"$source_text"; then
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

require_present 'ResolveDefensiveMarkTarget\(' \
    'raw child references must retain a defensive resolver'
require_present 'AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MANAGED>\([[:space:]]*ref, batch, noWork, publication\)' \
    'Runtime write barriers must use trusted ref-to-bitmap resolution'
require_present 'AddRefToMarkBatch<MSMarkRefSource::TRUSTED_MANAGED>\([[:space:]]*obj, batch, work, publication\)' \
    'EnumRoots publication must use trusted ref-to-bitmap resolution'

trusted_add="$(sed -n '/void MarkSweepCollector::AddRefToMarkBatch(/,/^}/p' "$COLLECTOR")"
require_absent "$trusted_add" '__atomic_load_n\(markWord|GetTypeInfo\(\)|collectEpoch' \
    'trusted add must not probe mark state or revalidate publication per ref'
require_present 'if \(LIKELY\(info->GetRegionKind\(\) == MSRegionKind::SMALL\)\)' \
    'trusted Small references must stay on the direct AddRefToMarkBatch path'
require_present 'AddMarkBitToBatch\(' \
    'trusted and defensive Small references must share the final batch insertion helper'
require_absent "$trusted_add" 'ResolveTrustedMarkTarget\(' \
    'trusted Small references must not materialize a generic resolved target'

require_present 'GetRegionInfoDirect\([[:space:]]*addr, regionOffset, regionIdx\)' \
    'ref resolution must retain the directly computed Region index'
require_present 'centralPool != nullptr' \
    'active mark epoch requires a CentralRegionPool invariant check'
require_present 'PublishRegionWords\(info, wordMask\)' \
    'bitmap flush must publish coalesced word masks per Region'
require_present 'PublishPendingMarkRegions\(publication\.pending\)' \
    'mark batch flush must drain pending Region publications'

flush_word="$(sed -n '/MSRegionInfo\* MarkSweepCollector::FlushMarkWordEntryImpl/,/^}/p' "$COLLECTOR")"
require_absent "$flush_word" 'markEpoch != MSRegionInfo::NO_COLLECT_EPOCH' \
    'active flush must rely on the nonzero mark-epoch invariant'
require_absent "$flush_word" 'PublishPendingMarkWord\(' \
    'each bitmap word must not publish the scheduler summary independently'

record_stats="$(sed -n '/void RecordMSBarrierRuntimeStat/,/^}/p' "$BARRIER")"
if ! rg -q 'if \(UNLIKELY\(IsMSBarrierRuntimeStatsEnabled\(\)\)\)' \
    <<<"$record_stats"; then
    echo 'barrier diagnostics must be gated by the stats enable flag' >&2
    exit 1
fi

pre_enum="$(sed -n '/void MarkSweepCollector::PreEnumRoots/,/^}/p' "$COLLECTOR")"
require_absent "$pre_enum" 'else \{[[:space:]]*// A RegionSpaceMS normally always has its central pool' \
    'PreEnumRoots must not open the active collector gate without preparing an epoch'

echo 'MSGC trusted mark-target contract holds'
