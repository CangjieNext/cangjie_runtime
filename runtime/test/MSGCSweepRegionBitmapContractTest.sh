#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REGION_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSRegionInfo.cpp"
CENTRAL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
TEST_SOURCE="$ROOT_DIR/test/MSRegionInfoEmptyRegionFastPathTest.cpp"
TEST_BINARY="${TMPDIR:-/tmp}/ms_region_sweep_bitmap_contract.$$"
trap 'rm -f "$TEST_BINARY"' EXIT

clang++ -std=c++14 -I"$ROOT_DIR" -I"$ROOT_DIR/src" "$TEST_SOURCE" -o "$TEST_BINARY"
"$TEST_BINARY"

python3 - "$REGION_SOURCE" "$CENTRAL_SOURCE" <<'PY'
import pathlib
import sys

region_source = pathlib.Path(sys.argv[1]).read_text()
central_source = pathlib.Path(sys.argv[2]).read_text()

def section(source, start_token, end_token):
    start = source.index(start_token)
    end = source.index(end_token, start)
    return source[start:end]

sweep_region = section(
    region_source,
    'MSCentralReturnDecision MSRegionInfo::SweepRegion()',
    '// ============================================================================\n// MSRegionInfo — Mark',
)
sweep_list = section(
    central_source,
    'bool CentralRegionPool::SweepUnsweptList(',
    '// ===== Monitoring =====',
)

if 'markWords[' in sweep_region:
    raise SystemExit('SweepRegion must read stable markBitmap directly without a stack copy')
if 'HasAnyValidMarkBits' not in sweep_region:
    raise SystemExit('SweepRegion must use an early-exit valid-mark probe before rebuilding the allocation plane')
probe_prefix = sweep_region.split('if (!HasAnyValidMarkBits', 1)[0]
if '__builtin_popcountll' in probe_prefix:
    raise SystemExit('the empty-Region probe must not popcount every mark word')
if 'validMarkBits' not in sweep_region or 'validMask' not in sweep_region:
    raise SystemExit('SweepRegion must mask invalid trailing mark bits before popcount and inversion')
if 'runStartSlot' not in sweep_region:
    raise SystemExit('SweepRegion must merge adjacent dead slots into runs across bitmap words')
if 'markBitmap' in sweep_region.split('liveCount == 0', 1)[-1] and 'memset(markBitmap' in sweep_region:
    raise SystemExit('SweepRegion must leave markBitmap clearing to the next MarkSetup')
if 'kSweepDeadlineCheckInterval' not in sweep_list or '% kSweepDeadlineCheckInterval' not in sweep_list:
    raise SystemExit('Sweep deadline must be sampled once per four swept Regions')
if 'kSweepDeadlineCheckInterval = 4' not in central_source:
    raise SystemExit('Sweep deadline sampling interval must be exactly four Regions')

print('MSGC SweepRegion bitmap/dead-run contract holds')
PY
