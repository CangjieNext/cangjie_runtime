#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h"
SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"

for symbol in \
    GetVirtualReserveSize \
    GetHeapMemorySize \
    GetMetadataSize \
    GetRegionCount \
    ReclaimEmptyRegionToHotFree; do
    if rg -q "$symbol" "$HEADER" "$SOURCE"; then
        echo "obsolete MSGC RegionManager API remains: $symbol" >&2
        exit 1
    fi
done

rg -q 'TryComputeVirtualReserveLayout' "$HEADER" "$SOURCE"
rg -q 'ReclaimReleasedRegionsToHotFreeBatch' "$HEADER" "$SOURCE"

echo 'MSGC RegionManager dead API cleanup contract: PASS'
