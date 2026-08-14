#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SESSION="$ROOT_DIR/src/Heap/MCollector/MSGCWorkerPoolSession.h"
CONTROLLER="$ROOT_DIR/src/Heap/MCollector/MSGCWorkerPoolController.h"
RESOURCES_HEADER="$ROOT_DIR/src/Heap/Collector/CollectorResources.h"
RESOURCES_SOURCE="$ROOT_DIR/src/Heap/Collector/CollectorResources.cpp"
CENTRAL="$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
COLLECTOR="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
BACKGROUND="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"

for file in "$SESSION" "$CONTROLLER" "$RESOURCES_HEADER" "$RESOURCES_SOURCE"; do
    [[ -f "$file" ]] || { echo "missing MSGC worker-pool session support: $file" >&2; exit 1; }
done

rg -q 'class MSGCWorkerPoolSession' "$SESSION"
rg -q 'class MSGCWorkerTaskBatch' "$SESSION"
rg -q 'class MSGCWorkerPoolController' "$CONTROLLER"
rg -q 'bool Acquire\(\)' "$SESSION"
rg -q 'void Start\(' "$SESSION"
rg -q 'void Finish\(\)' "$SESSION"
rg -q 'new \(std::nothrow\) HeapWork\*\[' "$SESSION"
if rg -q 'for \(size_t i = 0; i < capacity_ && tasks_ != nullptr; \+\+i\)' "$SESSION"; then
    echo 'MSGC task batch must not repeat value-initialization of its pointer array' >&2
    exit 1
fi
rg -q 'AcquireMSGCWorkerPool\(' "$RESOURCES_HEADER" "$RESOURCES_SOURCE"

for obsolete in \
    "$ROOT_DIR/src/Heap/Collector/MSGCWorkerPoolController.h" \
    "$ROOT_DIR/src/Heap/Collector/MSGCWorkerPoolSession.h"; do
    [[ ! -e "$obsolete" ]] || {
        echo "MSGC worker-pool support must not remain under Heap/Collector: $obsolete" >&2
        exit 1
    }
done

for file in "$CENTRAL" "$BACKGROUND"; do
    rg -q 'MSGCWorkerPoolSession' "$file"
done

if rg -q 'GCThreadPool\* GetThreadPool\(\) const' "$SESSION" ||
   rg -q 'sweepSession\.GetThreadPool\(\)' "$BACKGROUND"; then
    echo 'worker-pool session still leaks its owned thread pool' >&2
    exit 1
fi

if rg -q 'AcquireMSGCWorkerPoolForPostMarkStats' "$COLLECTOR"; then
    echo 'PostMark stats still open-codes its worker-pool lease' >&2
    exit 1
fi

if rg -q 'TryAcquireMSGCWorkerPoolForPreEnum' "$CENTRAL" ||
   rg -q 'TryAcquireMSGCWorkerPoolForSweep' "$BACKGROUND"; then
    echo 'A short MSGC worker-pool epoch still uses a phase-specific lease' >&2
    exit 1
fi

python3 - "$COLLECTOR" "$BACKGROUND" <<'PY'
import pathlib
import sys

collector = pathlib.Path(sys.argv[1]).read_text()
background = pathlib.Path(sys.argv[2]).read_text()

def block(source, begin, end):
    start = source.index(begin)
    finish = source.index(end, start)
    return source[start:finish]

for source, begin, end in (
    (collector, 'void MarkSweepCollector::DrainSchedulerWithHelpersUnderSTW()',
     'void MarkSweepCollector::VerifyMarkSchedulerQuiescence('),
    (collector, 'bool MarkSweepCollector::StartMarkWorkers(size_t maxHelpers)',
     'void MarkSweepCollector::RunMarkWorker(size_t workerId)'),
    (background, 'bool BackgroundProcessor::ParallelSweepLoop(size_t helperCount)',
     'bool BackgroundProcessor::CompleteSweepSlice('),
):
    body = block(source, begin, end)
    if 'MSGCWorkerTaskBatch stagedTasks' not in body:
        raise SystemExit(f'{begin} must stage tasks in the no-throw task batch')
    if 'std::vector<std::unique_ptr<HeapWork>> stagedTasks' in body or 'stagedTasks.reserve' in body:
        raise SystemExit(f'{begin} must not use a throwing vector reserve before serial fallback')

print('MSGC worker task staging contract: PASS')
PY

if ! rg -q 'std::unique_ptr<uint64_t\[\]> markWorkerCompletedWU_' "$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h" ||
   ! rg -q 'new \(std::nothrow\) uint64_t\[maxHelpers\]\(\)' "$COLLECTOR"; then
    echo 'Mark worker accounting must retain a no-throw serial fallback' >&2
    exit 1
fi

echo 'MSGC short worker-pool epochs use the shared session'
