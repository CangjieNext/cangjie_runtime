#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BASE_SOURCE="$ROOT_DIR/src/Common/BaseObject.cpp"
FINALIZER_SOURCE="$ROOT_DIR/src/Heap/Collector/FinalizerWorkList.cpp"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"

created="$(sed -n '/void BaseObject::OnFinalizerCreated/,/^}/p' "$BASE_SOURCE")"
msgc_created="$(printf '%s\n' "$created" | sed -n '/#ifdef MRT_USE_MARK_SWEEP/,/#else/p')"
printf '%s\n' "$msgc_created" | rg -q 'RegisterFinalizer\(this\)'
if printf '%s\n' "$msgc_created" | rg -q 'MarkNewObject\('; then
    echo 'BaseObject publishes outside the FinalizerWorkList lock' >&2
    exit 1
fi

register="$(sed -n '/void FinalizerWorkList::RegisterFinalizer/,/^}/p' "$FINALIZER_SOURCE")"
lock_line="$(printf '%s\n' "$register" | rg -n 'lock_guard<std::mutex>' | cut -d: -f1)"
insert_line="$(printf '%s\n' "$register" | rg -n 'finalizers\.push_back' | cut -d: -f1)"
publish_line="$(printf '%s\n' "$register" | rg -n 'MarkNewObject\(obj\)' | cut -d: -f1)"
if [[ -z "$lock_line" || -z "$insert_line" || -z "$publish_line" ||
      "$lock_line" -ge "$insert_line" || "$insert_line" -ge "$publish_line" ]]; then
    echo 'finalizer registration must insert then publish under listLock' >&2
    exit 1
fi

process="$(sed -n '/void FinalizerWorkList::ProcessFinalizableList/,/^}/p' "$FINALIZER_SOURCE")"
publish_line="$(printf '%s\n' "$process" | rg -n 'MarkNewObject\(finalizeObjAddr\)' | cut -d: -f1)"
erase_line="$(printf '%s\n' "$process" | rg -n 'workingFinalizables\.erase' | cut -d: -f1)"
if [[ -z "$publish_line" || -z "$erase_line" || "$publish_line" -ge "$erase_line" ]]; then
    echo 'working finalizer root must be published before removal' >&2
    exit 1
fi

termination="$(sed -n '/void MarkSweepCollector::MarkTermination/,/^}/p' "$COLLECTOR_SOURCE")"
ordinary_publish_line="$(printf '%s\n' "$termination" | rg -n 'PublishOrdinaryMarkTasksUnderSTW' | cut -d: -f1)"
ordinary_drain_line="$(printf '%s\n' "$termination" | rg -n 'DrainSchedulerWithHelpersUnderSTW' | head -n 1 | cut -d: -f1)"
roots_line="$(printf '%s\n' "$termination" | rg -n 'PublishFinalizerResurrectionRootsUnderSTW' | cut -d: -f1)"
closure_line="$(printf '%s\n' "$termination" | rg -n 'DrainSchedulerWithHelpersUnderSTW' | tail -n 1 | cut -d: -f1)"
weak_line="$(printf '%s\n' "$termination" | rg -n 'ClearWeakRefsAfterMark' | cut -d: -f1)"
finalizer_publish="$(sed -n '/void MarkSweepCollector::PublishFinalizerResurrectionRootsUnderSTW/,/^}/p' "$COLLECTOR_SOURCE")"
prepare_line="$(printf '%s\n' "$finalizer_publish" | rg -n 'PrepareFinalizersForResurrection' | cut -d: -f1)"
if [[ -z "$prepare_line" ||
      "$ordinary_publish_line" -ge "$ordinary_drain_line" ||
      "$ordinary_drain_line" -ge "$roots_line" ||
      "$roots_line" -ge "$closure_line" ||
      "$closure_line" -ge "$weak_line" ]]; then
    echo 'MarkTermination finalizer/weak ordering is invalid' >&2
    exit 1
fi

echo 'MSGC finalizer publication and resurrection ordering contract holds'
