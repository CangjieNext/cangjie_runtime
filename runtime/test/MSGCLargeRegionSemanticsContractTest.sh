#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

python3 - \
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp" \
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp" \
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h" <<'PY'
import pathlib
import sys

space = pathlib.Path(sys.argv[1]).read_text()
central = pathlib.Path(sys.argv[2]).read_text()
central_header = pathlib.Path(sys.argv[3]).read_text()

required_space = (
    'size_t objectBytes, size_t allocSize, MSScanClass scanClass,',
    'RegisterLargeObjectMS(addr, objectBytes, regionBytes, scanClass);',
    'return msCentralPool_->GetLargeObjectBytes();',
)
for token in required_space:
    if token not in space:
        raise SystemExit(f'Missing exact Large object/space semantic: {token}')

required_central = (
    'region->LargeSpanCount()',
    'largeNextSupply_.push_back',
    'largeCollectSet_.swap(largeNextSupply_)',
    'largeLiveObjectBytes_.fetch_add(region->LargeObjectSize()',
    'largeLiveObjectBytes_.fetch_sub(',
    'stats.AddLargeObject(region->LargeSpanBytes())',
    'reinterpret_cast<BaseObject*>(region->LargeObjectStart())',
)
for token in required_central:
    if token not in central:
        raise SystemExit(f'Missing Large central-pool semantic: {token}')

if central.count('region->LargeSpanCount()') < 2:
    raise SystemExit('Large span count must be used for both registration and removal')

if 'largeLiveObjectBytes_' not in central_header or 'GetLargeObjectBytes() const' not in central_header:
    raise SystemExit('CentralRegionPool must expose exact live Large object bytes')

for forbidden in (
    'region->regionEnd - region->RegionStart()',
    'reinterpret_cast<BaseObject*>(region->RegionStart())',
):
    if forbidden in central:
        raise SystemExit(f'Large path still uses small-Region semantics: {forbidden}')

print('MSGC Large Region semantics contract holds')
PY
