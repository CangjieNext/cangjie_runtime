#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
BASE_OBJECT_SOURCE="$ROOT_DIR/src/Common/BaseObject.cpp"
TRACING_SOURCE="$ROOT_DIR/src/Heap/Collector/TracingCollector.cpp"
RESOURCES_HEADER="$ROOT_DIR/src/Heap/Collector/CollectorResources.h"
RESOURCES_SOURCE="$ROOT_DIR/src/Heap/Collector/CollectorResources.cpp"
HEAP_DUMP_SOURCE="$ROOT_DIR/src/Inspector/CjHeapData.cpp"
FINALIZER_LIST_SOURCE="$ROOT_DIR/src/Heap/Collector/FinalizerWorkList.cpp"
BACKGROUND_SOURCE="$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"

python3 - "$HEADER" "$SOURCE" "$BASE_OBJECT_SOURCE" "$TRACING_SOURCE" "$RESOURCES_HEADER" \
    "$RESOURCES_SOURCE" "$HEAP_DUMP_SOURCE" "$FINALIZER_LIST_SOURCE" "$BACKGROUND_SOURCE" <<'PY'
from pathlib import Path
import sys


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise SystemExit(f"unterminated function: {signature}")


header = Path(sys.argv[1]).read_text(encoding="utf-8")
source = Path(sys.argv[2]).read_text(encoding="utf-8")
base_object_source = Path(sys.argv[3]).read_text(encoding="utf-8")
tracing_source = Path(sys.argv[4]).read_text(encoding="utf-8")
resources_header = Path(sys.argv[5]).read_text(encoding="utf-8")
resources_source = Path(sys.argv[6]).read_text(encoding="utf-8")
heap_dump_source = Path(sys.argv[7]).read_text(encoding="utf-8")
finalizer_list_source = Path(sys.argv[8]).read_text(encoding="utf-8")
background_source = Path(sys.argv[9]).read_text(encoding="utf-8")

on_finalizer_created = extract_function(base_object_source, "void BaseObject::OnFinalizerCreated()")
if "heap.GetCollectorResources().RegisterFinalizer(this);" not in on_finalizer_created:
    raise SystemExit("Finalizer registration must use the same CollectorResources routing as MSGC classification")
if "GetFinalizerProcessor().RegisterFinalizer" in on_finalizer_created:
    raise SystemExit("MSGC must not register Finalizers in the inactive FinalizerProcessor list")

enum_finalizer_roots = extract_function(tracing_source, "void TracingCollector::EnumFinalizerProcessorRoots")
if "collectorResources.VisitFinalizerGCRoots(visitor);" not in enum_finalizer_roots:
    raise SystemExit("CopyGC Finalizer root enumeration must use CollectorResources routing")
if "GetFinalizerProcessor().VisitGCRoots" in enum_finalizer_roots:
    raise SystemExit("Finalizer root enumeration must not directly select a processor list")
guard = enum_finalizer_roots.find("#ifndef MRT_USE_MARK_SWEEP")
visit = enum_finalizer_roots.find("collectorResources.VisitFinalizerGCRoots(visitor);")
guard_end = enum_finalizer_roots.find("#endif", guard)
if guard < 0 or not guard < visit < guard_end:
    raise SystemExit("MSGC must defer finalizable roots until the termination resurrection closure")

heap_dump_finalizers = extract_function(heap_dump_source, "void CjHeapData::ProcessRootFinalizer()")
if "GetCollectorResources().VisitFinalizerGCRoots(visitor);" not in heap_dump_finalizers:
    raise SystemExit("heap dump must visit the Finalizer list selected by CollectorResources")
if "GetFinalizerProcessor().VisitGCRoots" in heap_dump_finalizers:
    raise SystemExit("MSGC heap dump must not read the inactive FinalizerProcessor list")

for token in [
    "backgroundProcessor.RegisterFinalizer(obj);",
    "backgroundProcessor.VisitFinalizers(visitor);",
    "backgroundProcessor.VisitGCRoots(visitor);",
    "backgroundProcessor.EnqueueFinalizables(finalizable, countLimit);",
    "backgroundProcessor.NotifyFinalizerWork();",
]:
    if token not in resources_header:
        raise SystemExit(f"CollectorResources MSGC Finalizer routing is incomplete: {token}")

notify_gc_finished = extract_function(resources_source, "void CollectorResources::NotifyGCFinished")
clear_gc_started = notify_gc_finished.find("isGcStarted.store(false, std::memory_order_release);")
resume_finalizers = notify_gc_finished.find("backgroundProcessor.NotifyFinalizerWork();")
if clear_gc_started < 0 or resume_finalizers < clear_gc_started:
    raise SystemExit("GC completion must reopen and wake MSGC Finalizer processing after clearing isGcStarted")

process_finalizable_list = extract_function(
    finalizer_list_source, "void FinalizerWorkList::ProcessFinalizableList")
mark_completed = process_finalizable_list.find("Heap::GetHeap().GetCollector().MarkNewObject(finalizeObjAddr);")
erase_completed = process_finalizable_list.find("workingFinalizables.erase(itor)")
if mark_completed < 0 or erase_completed < mark_completed:
    raise SystemExit("a Finalizer completing during MSGC mark must be shaded before its working root is removed")
mark_guard = process_finalizable_list.rfind("#ifdef MRT_USE_MARK_SWEEP", 0, mark_completed)
mark_guard_end = process_finalizable_list.find("#endif", mark_completed)
if mark_guard < 0 or mark_guard_end < mark_completed:
    raise SystemExit("the completion shading hook must remain MSGC-only")

process_finalizables = extract_function(
    finalizer_list_source, "void FinalizerWorkList::ProcessFinalizables")
resume_working = process_finalizables.find("if (workingFinalizables.empty())")
swap_new_batch = process_finalizables.find("workingFinalizables.swap(finalizables)")
if resume_working < 0 or not resume_working < swap_new_batch:
    raise SystemExit("an interrupted working Finalizer batch must resume before accepting a new batch")
if "!finalizables.empty() || !workingFinalizables.empty()" not in process_finalizables:
    raise SystemExit("Finalizer runnable state must include both queued and interrupted working entries")

background_run = extract_function(background_source, "void BackgroundProcessor::Run()")
gc_finalizer_gate = "!Heap::GetHeap().GetCollectorResources().IsGcStarted()"
if background_run.count(gc_finalizer_gate) < 2:
    raise SystemExit("BackgroundProcessor must gate both Finalizer batch entry and per-object continuation during MSGC")
background_pending = extract_function(background_source, "bool BackgroundProcessor::HasPendingWakeWork() const")
if gc_finalizer_gate not in background_pending:
    raise SystemExit("paused Finalizer work must not keep BackgroundProcessor spinning during MSGC")

if "ProcessDeferredWeakRefsPreSTW" in header or "ProcessDeferredWeakRefsPreSTW" in source:
    raise SystemExit("WeakRefs must not be classified before the MarkTermination STW fixed point")

for signature in [
    "void MarkSweepCollector::RunRegionTask",
    "void MarkSweepCollector::RunObjectTask",
]:
    task = extract_function(source, signature)
    weak_start = task.index("if (UNLIKELY(typeInfo->IsWeakRefType()))")
    weak_branch_end = task.index("} else", weak_start)
    weak_branch = task[weak_start:weak_branch_end]
    if "weakRefs.Add(obj);" not in weak_branch:
        raise SystemExit(
            f"{signature} must stage live WeakRef objects in its worker-local batch"
        )
    if "ForEachRefField" in weak_branch or "AddRefToMarkBatch" in weak_branch:
        raise SystemExit("a WeakRef referent must not be traced as a strong edge")

termination = extract_function(source, "void MarkSweepCollector::MarkTermination(bool worldStopped)")
ordered = [
    "PublishOrdinaryMarkTasksUnderSTW(*centralPool);",
    "DrainSchedulerWithHelpersUnderSTW();",
    "PublishFinalizerResurrectionRootsUnderSTW(",
    "DrainSchedulerWithHelpersUnderSTW();",
    "ClearWeakRefsAfterMark();",
    "VerifyMarkCompleteness();",
    "TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, false);",
]
cursor = 0
for token in ordered:
    found = termination.find(token, cursor)
    if found < 0:
        raise SystemExit(f"MarkTermination missing ordered post-mark semantic step: {token}")
    cursor = found + len(token)

finalizer_publish = extract_function(
    source, "void MarkSweepCollector::PublishFinalizerResurrectionRootsUnderSTW(")
if "PrepareFinalizersForResurrection();" not in finalizer_publish:
    raise SystemExit("finalizer classification must precede resurrection-root publication")

prepare_finalizers = extract_function(source, "void MarkSweepCollector::PrepareFinalizersForResurrection()")
enqueue = prepare_finalizers.find("collectorResources.EnqueueFinalizables")
if enqueue < 0:
    raise SystemExit("Finalizers must be snapshotted and classified while their original mark state is intact")
if "return !IsMarkedObject(obj);" not in prepare_finalizers:
    raise SystemExit("only unreachable registered Finalizers may enter the resurrection set")

clear_weak = extract_function(source, "void MarkSweepCollector::ClearWeakRefsAfterMark(")
for token in [
    "WeakRefBuffer::Instance().DrainWeakRefs",
    "IsMarkedObject(referent)",
    "MSStoreRawReference(*referentField, nullptr)",
]:
    if token not in clear_weak:
        raise SystemExit(f"STW WeakRef cleanup is missing: {token}")

post_mark = extract_function(source, "void MarkSweepCollector::PostMarkTermination()")
if "ClearWeakRefBuffer" in post_mark or "ClearWeakRefsAfterMark" in post_mark:
    raise SystemExit("WeakRef cleanup must finish under STW, not in PostMarkTermination")

if "void MarkSweepCollector::ProcessFinalizers()" in source:
    raise SystemExit("MSGC must not retain a redundant PostMark Finalizer notification hook")
if "backgroundProcessor.NotifyFinalizerWork();" not in resources_source:
    raise SystemExit("NotifyGCFinished must wake Finalizer work after reopening GC admission")
PY

echo 'MSGC post-mark semantic closure contract holds'
