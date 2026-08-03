#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FILES=(
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.h"
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionManager.cpp"
    "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
    "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
)

if rg -n 'ReenableLazySweep|skipLazySweep_|RecordLazySweep|MaybeNotifySweep|lazy[ -]?sweep' "${FILES[@]}"; then
    echo "MSGC must not retain inactive Lazy Sweep APIs, state, or comments" >&2
    exit 1
fi

if ! rg -q 'void ReenableSweepAdmission\(\)' \
        "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h" ||
   ! rg -q 'void CentralRegionPool::ReenableSweepAdmission\(\)' \
        "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"; then
    echo "post-Mark must expose the accurately named Sweep admission reopen API" >&2
    exit 1
fi

echo "MSGC no-Lazy-Sweep-remnants contract: PASS"
