#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
CENTRAL_HEADER="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
CENTRAL_SOURCE="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"

ENUM_BODY="$(sed -n \
    '/void MarkSweepCollector::EnumAndMarkRoots()/,/^}/p' \
    "$COLLECTOR")"
if rg -q 'MutatorManagementWLock|MutatorManagementWUnlock' <<<"$ENUM_BODY"; then
    echo 'EnumAndMarkRoots must not reacquire the mutator-management write lock' >&2
    exit 1
fi
rg -q 'MergeRootsFromAllocBuffers\(rootStaging_\)' <<<"$ENUM_BODY"

VISIT_BODY="$(sed -n \
    '/void VisitAllocBufferMS(Visitor&& visitor)/,/^    }/p' \
    "$CENTRAL_HEADER")"
rg -q 'lock_guard<std::mutex> lg\(msAllocBufferLock_\)' <<<"$VISIT_BODY"

REGISTER_BODY="$(sed -n \
    '/void CentralRegionPool::RegisterAllocBufferMS/,/^}/p' \
    "$CENTRAL_SOURCE")"
REMOVE_BODY="$(sed -n \
    '/void CentralRegionPool::RemoveAllocBufferMS/,/^}/p' \
    "$CENTRAL_SOURCE")"
rg -q 'lock_guard<std::mutex> lg\(msAllocBufferLock_\)' <<<"$REGISTER_BODY"
rg -q 'lock_guard<std::mutex> lg\(msAllocBufferLock_\)' <<<"$REMOVE_BODY"

echo 'MSGC EnumRoots merges roots under the AllocBufferMS registry lifetime lock'
