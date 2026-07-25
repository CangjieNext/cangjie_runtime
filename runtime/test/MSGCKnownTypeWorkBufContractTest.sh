#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCAN_HEADER="$ROOT_DIR/src/Common/GCRefFieldScan.inline.h"
COLLECTOR_HEADER="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.h"
COLLECTOR_SOURCE="$ROOT_DIR/src/Heap/MCollector/MarkSweepCollector.cpp"
TRACING_SOURCE="$ROOT_DIR/src/Heap/Collector/TracingCollector.cpp"

python3 - "$SCAN_HEADER" "$COLLECTOR_HEADER" "$COLLECTOR_SOURCE" "$TRACING_SOURCE" <<'PY'
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


scan = Path(sys.argv[1]).read_text(encoding="utf-8")
collector_header = Path(sys.argv[2]).read_text(encoding="utf-8")
collector = Path(sys.argv[3]).read_text(encoding="utf-8")
tracing = Path(sys.argv[4]).read_text(encoding="utf-8")

if "ForEachRefFieldGCKnownType(BaseObject* obj, TypeInfo* typeInfo" not in scan:
    raise SystemExit("GC scanner must expose a known-TypeInfo trusted-grey entry")
known_scan = extract_function(scan, "inline void ForEachRefFieldGCKnownType")
if "typeInfo->HasRefField()" in known_scan:
    raise SystemExit("known-TypeInfo scanner must not repeat the producer HasRef gate")
if "mArray->GetElementSize()" in scan:
    raise SystemExit("GC array scanner must hoist element size instead of re-reading TypeInfo per element")

if "bool DiscoverObject(BaseObject* obj, TypeInfo** markedTypeInfo = nullptr)" not in collector_header:
    raise SystemExit("DiscoverObject must optionally return the validated winner TypeInfo")

discover = extract_function(collector, "bool MarkSweepCollector::DiscoverObject(BaseObject* obj, TypeInfo** markedTypeInfo)")
for token in ["TypeInfo* typeInfo = obj->GetTypeInfo();", "target.TryMark()", "*markedTypeInfo = typeInfo;"]:
    if token not in discover:
        raise SystemExit(f"DiscoverObject does not reuse validated TypeInfo: {token}")
if "obj->IsValidObject()" in discover:
    raise SystemExit("DiscoverObject must not reload stateWord after obtaining the validated TypeInfo")

enqueue = extract_function(
    collector,
    "bool MarkSweepCollector::TryEnqueueGreyObject(BaseObject* obj, GreyWorkSink& sink)",
)
if "DiscoverObject(obj, &typeInfo)" not in enqueue or "typeInfo->HasRefField()" not in enqueue:
    raise SystemExit("grey producer must filter with the TypeInfo returned by DiscoverObject")
if "obj->HasRefField()" in enqueue:
    raise SystemExit("grey producer must not reload the winner stateWord")

trace = extract_function(
    collector,
    "void MarkSweepCollector::TraceObjectRefFieldsToGCWork(BaseObject* obj, TypeInfo* typeInfo,",
)
if "ForEachRefFieldGCKnownType(obj, typeInfo, refFunc)" not in trace:
    raise SystemExit("WorkBuf tracing must use the cached known-TypeInfo scanner")
if "target->HasRefField()" in trace:
    raise SystemExit("child enqueue must reuse DiscoverObject's winner TypeInfo")

drain = extract_function(
    collector,
    "size_t MarkSweepCollector::DrainCurrentBuf(PerPGCWork& gcw)",
)
if "TypeInfo* typeInfo = obj->GetTypeInfo();" not in drain:
    raise SystemExit("DrainCurrentBuf must load each grey object's TypeInfo once")
if "obj->HasRefField()" in drain:
    raise SystemExit("Release WorkBuf consumer must trust the producer HasRef contract")
if "typeInfo->IsWeakRefType()" not in drain:
    raise SystemExit("WeakRef classification must reuse the cached TypeInfo")

resolved = extract_function(collector, "struct ResolvedMarkTarget")
if "IsMarkBitSetResolved" not in resolved or "MarkBitmapWordResolved" not in resolved:
    raise SystemExit("resolved mark targets must use unchecked validated-coordinate bitmap operations")

resurrected = extract_function(tracing, "void TracingCollector::EnumAllSurrectedExportRoots")
guard = resurrected.find("#ifdef MRT_USE_MARK_SWEEP")
enum_call = resurrected.find("EnumAndTagRawRoot(root, rootSet);")
guard_end = resurrected.find("#else", guard)
if guard < 0 or enum_call < guard or guard_end < enum_call:
    raise SystemExit("MSGC resurrected export roots must enter through EnumAndTagRawRoot")

print("MSGC known-TypeInfo and trusted-grey WorkBuf contract holds")
PY
