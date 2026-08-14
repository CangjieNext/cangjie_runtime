#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ACCOUNTING_H="$ROOT_DIR/src/Heap/MCollector/MSHeapAccounting.h"
ACCOUNTING_CPP="$ROOT_DIR/src/Heap/MCollector/MSHeapAccounting.cpp"
MCOLLECTOR_CMAKE="$ROOT_DIR/src/Heap/MCollector/CMakeLists.txt"
SPACE_H="$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.h"
SPACE_CPP="$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
MANAGER_CPP="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
BACKGROUND_CPP="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"
COLLECTOR_CPP="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "missing MSGC heap-accounting file: $1" >&2
        exit 1
    fi
}

require_pattern() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! rg -q "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_absent() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if rg -q "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_file "$ACCOUNTING_H"
require_file "$ACCOUNTING_CPP"

for type in MSHeapRetentionSnapshot MSSweepProgressSnapshot MSHeapPolicySnapshot; do
    require_pattern "$ACCOUNTING_H" "struct $type" \
        "MSHeapAccounting must define $type"
done

for field in activeBytes emptyCacheBytes hotFreeBytes; do
    require_pattern "$ACCOUNTING_H" "$field" \
        "retention snapshot must expose $field"
done

for symbol in CaptureMSHeapRetentionSnapshot \
              CaptureMSHeapRetentionDiagnosticSnapshot \
              CaptureMSSweepProgressSnapshot \
              LoadMSHeapPolicySnapshot \
              ComputeMSScavengeReleaseTarget \
              EvaluateMSMarkTrigger; do
    require_pattern "$ACCOUNTING_H" "$symbol" \
        "MSHeapAccounting must expose $symbol"
    require_pattern "$ACCOUNTING_CPP" "$symbol" \
        "MSHeapAccounting must implement $symbol"
done

require_pattern "$ACCOUNTING_CPP" 'GetHotFreeBytesReference' \
    'the detailed reference-retention snapshot must retain the FreeManager scan'
require_absent "$ACCOUNTING_H" 'CaptureMSHeapRetentionReferenceSnapshot' \
    'the partial hot-free reference scan must not be presented as a full reference snapshot'
require_pattern "$ACCOUNTING_H" 'struct MSMarkTriggerDecision' \
    'Mark-trigger evaluation must publish its decision inputs for diagnostics'
require_pattern "$MCOLLECTOR_CMAKE" 'MSHeapAccounting.cpp' \
    'MCollector must build MSHeapAccounting.cpp'

for retired in GetHeapInUseBytes GetSweptHeapBytes GetCentralCacheBytes \
               GetHeapRetainedBytes GetHeapThresholdBytes GetScavengeThresholdBytes \
               GetScavengeReleaseTargetBytes HasPendingScavengeWork; do
    require_absent "$SPACE_H" "$retired" \
        "RegionSpace must not retain heap-accounting query $retired"
    require_absent "$SPACE_CPP" "RegionSpaceMS::$retired" \
        "RegionSpace must not implement heap-accounting query $retired"
done

for caller in "$MANAGER_CPP" "$BACKGROUND_CPP" "$COLLECTOR_CPP"; do
    require_pattern "$caller" 'CaptureMSHeapRetentionSnapshot' \
        "heap-accounting caller must capture a local retention snapshot: $caller"
done

require_pattern "$MANAGER_CPP" 'EvaluateMSMarkTrigger' \
    'Mark notification must use the shared mark-trigger calculation'
require_pattern "$BACKGROUND_CPP" 'ComputeMSScavengeReleaseTarget' \
    'Background Scavenge must use the shared release-target calculation'

python3 - "$ACCOUNTING_H" "$ACCOUNTING_CPP" "$MANAGER_CPP" "$BACKGROUND_CPP" <<'PY'
import pathlib
import sys

header, accounting, manager, background = (
    pathlib.Path(path).read_text() for path in sys.argv[1:])

if 'LoadMSHeapPolicySnapshot(const GCStats& gcStats);' not in header:
    raise SystemExit('policy snapshot must own the soft-limit read behind a one-argument API')

capture_start = accounting.index('MSSweepProgressSnapshot CaptureMSSweepProgressSnapshot')
capture_end = accounting.index('MSHeapPolicySnapshot LoadMSHeapPolicySnapshot', capture_start)
capture = accounting[capture_start:capture_end]
if 'GetTotalUnsweptBytes()' in capture:
    raise SystemExit('Sweep progress must not load unswept count twice through GetTotalUnsweptBytes')
if 'MSRegionsToBytes(snapshot.unsweptRegions)' not in capture:
    raise SystemExit('Sweep progress bytes must be derived from the captured unswept-region count')

def function_body(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise SystemExit(f'unterminated function: {signature}')

notify_scavenge = function_body(manager, 'void MSRegionManager::MaybeNotifyScavenge()')
probe = notify_scavenge.index('compare_exchange_strong')
recapture = notify_scavenge.index('CaptureMSHeapRetentionSnapshot(space)', probe)
repolicy = notify_scavenge.index('LoadMSHeapPolicySnapshot(gcStats)', probe)
decision = notify_scavenge.index('ComputeMSScavengeReleaseTarget', probe)
if not (probe < recapture < repolicy < decision):
    raise SystemExit('Scavenge probe owner must re-capture retention and policy before deciding to wake')

notify_mark = function_body(manager, 'void MSRegionManager::MaybeNotifyMark()')
if 'EvaluateMSMarkTrigger(retention, sweep, policy)' not in notify_mark:
    raise SystemExit('Mark notification must consume the shared trigger decision')
if 'SaturatingSubtractMSBytes(sweptBytes, lastMarkOutsideBytes)' in notify_mark:
    raise SystemExit('Mark notification must not recompute effective swept bytes outside the shared decision')

scavenge_loop = function_body(background, 'bool BackgroundProcessor::BackgroundScavengeLoop()')
if 'MSRegionManager* regionManager = space.GetMSRegionManager();' not in scavenge_loop:
    raise SystemExit('Background Scavenge must capture the RegionManager lifetime once')
if 'if (regionManager == nullptr)' not in scavenge_loop:
    raise SystemExit('Background Scavenge must stop safely when RegionManager is unavailable')
if 'regionManager->GetSegmentSizeBytes()' not in scavenge_loop:
    raise SystemExit('Background Scavenge must use its validated RegionManager')

periodic_scavenge = function_body(
    background, 'bool BackgroundProcessor::ShouldStartPeriodicScavenge() const')
if 'space.GetMSRegionManager() == nullptr' not in periodic_scavenge:
    raise SystemExit('Periodic Scavenge must not request work without a RegionManager')

import re
for name, source in [('MSRegionManager', manager), ('BackgroundProcessor', background)]:
    if re.search(r'LoadMSHeapPolicySnapshot\(\s*[^)]*CangjieRuntime::GetMSSoftHeapLimitBytes\(\)', source):
        raise SystemExit(f'{name} must not pass the soft limit into every policy capture')
PY

echo 'MSGC heap accounting contract holds'
