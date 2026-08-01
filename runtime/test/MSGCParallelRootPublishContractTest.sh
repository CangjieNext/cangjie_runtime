#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
publish="$(sed -n '/void MarkSweepCollector::PublishRootStaging/,/^}/p' "$COLLECTOR")"
printf '%s\n' "$publish" | rg -q 'std::vector<BaseObject\*>& roots = rootStaging_'
printf '%s\n' "$publish" | rg -q 'rawRootCount = roots\.size\(\)'
printf '%s\n' "$publish" | rg -q 'rootDedupTable_\.Insert'
printf '%s\n' "$publish" | rg -q 'PublishRootRange'
printf '%s\n' "$publish" | rg -q 'PublishRootRange\(roots, 0, uniqueRootCount\)'
printf '%s\n' "$publish" | rg -q 'roots\.clear'
if printf '%s\n' "$publish" | rg -q 'RunStage|WaitStage|enumRootsWorkerSession_'; then
    echo 'pure-assist root publication must stay on the GC main thread' >&2
    exit 1
fi

range="$(sed -n '/void MarkSweepCollector::PublishRootRange/,/^}/p' "$COLLECTOR")"
printf '%s\n' "$range" | rg -q 'MSRootMarkWordBatch batch'
printf '%s\n' "$range" | rg -q 'FlushMarkBatchImpl\(batch, work\)'

echo 'MSGC root publish is serial and keeps dedup plus the GC-owned batch'
