#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
MUTATOR_SOURCE="$ROOT_DIR/src/Mutator/MutatorManager.cpp"
MUTATOR_HEADER="$ROOT_DIR/src/Mutator/MutatorManager.h"

rg -q 'GetLastMSEnumRootsHelperCount' "$MUTATOR_HEADER"
rg -q 'lastMSEnumRootsHelperCount_\.store\(helperCount' "$MUTATOR_SOURCE"

publish="$(sed -n '/void MarkSweepCollector::PublishRootStaging/,/^}/p' "$COLLECTOR")"
printf '%s\n' "$publish" | rg -q 'std::vector<BaseObject\*>& roots = rootStaging_'
printf '%s\n' "$publish" | rg -q 'rawRootCount = roots\.size\(\)'
printf '%s\n' "$publish" | rg -q 'rootDedupTable_\.Insert'
printf '%s\n' "$publish" | rg -q 'enumRootsWorkerSession_'
printf '%s\n' "$publish" | rg -q 'PublishRootRange'
printf '%s\n' "$publish" | rg -q 'RunStage'
printf '%s\n' "$publish" | rg -q 'WaitStage'
printf '%s\n' "$publish" | rg -q 'roots\.clear'

range="$(sed -n '/void MarkSweepCollector::PublishRootRange/,/^}/p' "$COLLECTOR")"
printf '%s\n' "$range" | rg -q 'MSRootMarkWordBatch batch'
printf '%s\n' "$range" | rg -q 'FlushMarkBatchImpl\(batch, work\)'

echo 'MSGC root publish reuses Enum helper width with per-thread batches'
