// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "HeapVerifier.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <set>
#include <string>
#include <utility>

#include "Allocator/Allocator.h"
#include "Allocator/RegionInfo.h"
#include "Allocator/RegionSpace.h"
#include "Base/Log.h"
#include "Base/Panic.h"
#include "Base/TimeUtils.h"
#include "Collector/FinalizerProcessor.h"
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {

namespace {
// "1"/"true"/"on" (case-insensitive) => true; unset/anything else => false.
bool IsTruthyEnv(const char* name)
{
    const char* val = std::getenv(name);
    if (val == nullptr) {
        return false;
    }
    auto equalsIgnoreCase = [val](const char* literal) {
        size_t i = 0;
        for (; literal[i] != '\0'; ++i) {
            char c = val[i];
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != literal[i]) {
                return false;
            }
        }
        return val[i] == '\0';
    };
    return equalsIgnoreCase("1") || equalsIgnoreCase("true") || equalsIgnoreCase("on");
}
} // namespace

bool HeapVerifier::IsPreVerifyEnabled()
{
    static const bool enabled = IsTruthyEnv("cjPreVerify");
    return enabled;
}

bool HeapVerifier::IsPostVerifyEnabled()
{
    static const bool enabled = IsTruthyEnv("cjPostVerify");
    return enabled;
}

bool HeapVerifier::IsPathTraceEnabled()
{
    static const bool enabled = IsTruthyEnv("cjVerifyRootPath");
    return enabled;
}

void HeapVerifier::Verify()
{
    MRT_ASSERT(MutatorManager::Instance().WorldStopped(), "HeapVerifier::Verify() requires STW");
    startTime_ = TimeUtil::NanoSeconds();
    Heap& heap = Heap::GetHeap();
    // The walker navigates without reading FP's write set, so it runs under
    // plain STW with FP's metadata jobs (reclaim/feed) executing freely.
    WalkHeap();
    VerifyRoots();
    if (failCount_ > 0) {
        FinalizerProcessor& fp = heap.GetFinalizerProcessor();
        const uint64_t finalizedNow = fp.GetFinalizedCount();
        const uint64_t enqueuedNow = fp.GetEnqueuedCount();
        LOG(RTLOG_ERROR,
            "FP activity this cycle: finalized %llu (from %llu to %llu), enqueued %llu, FP mid-run: %s",
            static_cast<unsigned long long>(finalizedNow - fpSnapshot_.finalizedCount),
            static_cast<unsigned long long>(fpSnapshot_.finalizedCount),
            static_cast<unsigned long long>(finalizedNow),
            static_cast<unsigned long long>(enqueuedNow - fpSnapshot_.enqueuedCount),
            fp.IsProcessing() ? "yes" : "no");
        if (std::strcmp(tag_, "pre-GC") == 0) {
            LOG(RTLOG_ERROR, "NOTE: heap was already corrupted BEFORE this GC cycle started");
        }
        // Trace root paths only after both WalkHeap and VerifyRoots returned:
        // failures may fire inside a Visit*Roots callback and the BFS re-iterates
        // those same root sources — re-entering the iteration from within its own
        // callback deadlocks (verified by fault injection).
        FlushPathTraces();
        CHECK_DETAIL(false, "%s heap verify failed with %zu failures", tag_, failCount_);
    }
    VLOG(REPORT, "%s heap verify passed: %zu objects, %zu roots, %llu us", tag_, objCount_, rootCount_,
         static_cast<unsigned long long>((TimeUtil::NanoSeconds() - startTime_) / 1000));
}

// Walker (spec 2026-09-10-heap-verify-walker-design.md section 3).
//
// Navigates the unit array from regionHeapStart to inactiveZone without
// reading any field in FP's write set (non-0-suffix metadata of garbage/free
// units being rewritten by ReclaimGarbageRegions/FeedHungryBuffers), which
// eliminates the FP handshake entirely.
//
// Load-bearing invariants (spec section 4):
//  1. InitRegionInfo writes regionEnd before SetUnitRole (role last).
//  2. Free units are always 1-unit (ToFreeRegion -> InitFreeRegion(idx, 1)).
//  3. 0-suffix fields + ghost bit are never written by FP (they must survive
//     unit recycling for lazy routing until next cycle's DispelGhostFromRegion).
//  4. FP's metadata jobs allocate no objects (FP-as-mutator parks at STW).
//  5. Torn reads only affect navigation (skip/advance), never the verdict:
//     object checks happen exclusively inside VisitAllObjects on regions that
//     passed the garbage/free/valid checks.
void HeapVerifier::WalkHeap()
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& regionManager = space.GetRegionManager();

    uintptr_t cursor = regionManager.GetRegionHeapStart();
    // Read once: FP may advance inactiveZone concurrently (TakeRegion fetch_add);
    // a stale (smaller) limit only skips freshly assembled regions, which hold
    // new objects with no mark bitmap — zero verification value either way.
    uintptr_t limit = regionManager.GetInactiveZone();

    while (cursor < limit) {
        // Branch 1: ghost bit. GetGhostFromRegionAt reads only the ghost bit
        // and 0-suffix fields (unitRole0/ownerRegion0), all frozen at
        // PrepareForwardableRegion and never written by FP. Cross the whole
        // region via the frozen extent (regionEnd0). Skipped content is
        // corpses or objects newly allocated on recycled units — both zero
        // value (corpses: ART heap.cc:3328 precedent; new: no bitmap).
        RegionInfo* ghostLeader = RegionInfo::GetGhostFromRegionAt(cursor);
        if (ghostLeader != nullptr) {
            uintptr_t next = ghostLeader->GetRegionStart() + ghostLeader->GetGhostRegionSize();
            cursor = (next > cursor) ? next : cursor + RegionInfo::UNIT_SIZE; // defensive on torn extent
            continue;
        }

        // Branch 2: single non-0 read (unitRole, via the safe helper that
        // never follows ownerRegion). FREE and SUBORDINATE units crawl one
        // unit at a time — this structurally eliminates the
        // InitSubordinateUnit mid-state SEGV (role is written before owner;
        // a null/stale owner is never read). Free units are always 1-unit
        // (invariant 2), so +UNIT_SIZE is exact.
        RegionInfo::UnitRole role = RegionInfo::GetUnitRoleAt(cursor);
        if (role == RegionInfo::UnitRole::FREE_UNITS || role == RegionInfo::UnitRole::SUBORDINATE_UNIT) {
            cursor += RegionInfo::UNIT_SIZE;
            continue;
        }

        // Branch 3: leader role. Invariant 1 (regionEnd written before role)
        // makes reading a leader role imply a complete extent. A region under
        // concurrent FP reclaim is skipped by type: GC thread set GARBAGE
        // before FP processes it, and FP's InitRegionInfo overwrites type to
        // FREE before role — every interleaving yields garbage/free/valid,
        // all handled. VisitAllObjects reads only this region's own fields
        // and object pages (stable under STW for valid regions).
        RegionInfo* region = RegionInfo::GetLeaderRegionInfoAt(cursor);
        uintptr_t regionEnd = region->GetRegionEnd();
        if (regionEnd > cursor && region->IsValidRegion() && !region->IsFreeRegion() &&
            !region->IsGarbageRegion()) {
            region->VisitAllObjects([this](BaseObject* obj) { VerifyObject(obj); });
        }
        cursor = (regionEnd > cursor) ? regionEnd : cursor + RegionInfo::UNIT_SIZE; // defensive
    }
}

void HeapVerifier::VerifyObject(BaseObject* obj)
{
    ++objCount_;
    if (!Heap::IsHeapAddress(reinterpret_cast<MAddress>(obj))) {
        RecordFailure("object %p from ForEachObj is outside heap range", obj);
        MaybeTraceRootPath("object", obj);
        return;
    }
    if (!obj->IsValidObject()) {
        RecordFailure("object %p has invalid header (null or corrupted TypeInfo)", obj);
        MaybeTraceRootPath("object", obj);
        return; // header untrusted: skip ref-field traversal (spec 6.1)
    }
    if (!CheckStateCode(obj)) {
        RecordFailure("object %p<%s> has unexpected state %d outside ghost from-region", obj,
                      obj->GetTypeInfo()->GetName(), static_cast<int>(obj->GetStateWord().GetStateCode()));
        MaybeTraceRootPath("object", obj);
        // state failure does not preclude ref traversal (spec 6.1)
    }
    if (RegionInfo::InGhostFromRegion(obj)) {
        return; // corpse: header+state already checked; its fields are pre-forwarding
                // stale snapshots that may legally reference dead/unroutable objects
                // (ART precedent heap.cc:3328 — do not verify garbage objects' refs).
    }
    // Bitmap gate (spec 6.1 rule 5): only live (marked) objects get ref-field checks.
    // Floating garbage in exempted regions legally holds stale tagged refs (tag
    // wraparound, dead targets) — production never reads them; neither do we.
    // Header+state checks above still apply to everything.
    if (!Heap::GetHeap().IsSurvivedObject(obj)) {
        return;
    }
    if (obj->GetTypeInfo()->HasRefField()) {
        obj->ForEachRefField([this, obj](RefField<>& field) { VerifyReference(obj, field); });
    }
}

void HeapVerifier::VerifyReference(BaseObject* holder, RefField<>& field)
{
    CheckTarget("ref-field", holder,
                static_cast<size_t>(reinterpret_cast<MAddress>(&field) - reinterpret_cast<MAddress>(holder)),
                ResolveRef(field));
}

// Resolve a ref-field target the way the runtime reads it between GC cycles
// (read-only version of IdleBarrier::ReadReference / TryUpdateRefFieldImpl,
// IdleBarrier.cpp:19-40, WCollector.cpp:57-96): untagged -> raw; tagged ->
// forward-table lookup, raw-address fallback when unroutable (TryUntagRefField
// semantics). Never writes/CASes. NOTE: tagged refs are NOT distinguished by
// current/old era here — between cycles BOTH carry pre-forwarding addresses.
BaseObject* HeapVerifier::ResolveRef(RefField<>& field) const
{
    RefField<> oldField(field);
    if (!oldField.IsTagged()) {
        return oldField.GetTargetObject();
    }
    BaseObject* fromObj = oldField.GetTargetObject();
    BaseObject* toObj = Heap::GetHeap().GetCollector().FindToVersion(fromObj);
    return (toObj != nullptr) ? toObj : fromObj;
}

bool HeapVerifier::CheckTarget(const char* kind, BaseObject* holder, size_t offset, BaseObject* ref)
{
    if (ref == nullptr) {
        return true; // legal null
    }
    if (!Heap::IsHeapAddress(reinterpret_cast<MAddress>(ref))) {
        // Non-heap target: distinguish a legal constant object (embedded in a
        // loaded module's data section, e.g. RawArray<UInt8> string literals)
        // from a corrupted reference via dladdr() — a pure address-range check
        // against file-backed module mappings, NO dereference of ref (which
        // would SEGV on a truly wild pointer to unmapped memory).
        Dl_info info;
        if (dladdr(reinterpret_cast<void*>(ref), &info) == 0) {
            // Not in any loaded module's mapping: stack / malloc / anonymous
            // mmap / unmapped — a heap ref must never point here.
            // Diagnostic: dump the mapping via /proc/self/maps for triage.
            RecordFailure("%s: %p<%s> offset %zu targets non-module %p (wild pointer?)", kind, holder,
                          (holder != nullptr) ? holder->GetTypeInfo()->GetName() : "root", offset, ref);
            DumpAddressMapping(reinterpret_cast<void*>(ref));
            MaybeTraceRootPath("holder", holder); // wild target is unreachable; the holder path is the lead
            return false;
        }
        return true; // constant object in a loaded module's data section
    }
    if (!ref->IsValidObject()) {
        RecordFailure("%s: %p<%s> offset %zu targets %p with invalid header", kind, holder,
                      (holder != nullptr) ? holder->GetTypeInfo()->GetName() : "root", offset, ref);
        MaybeTraceRootPath("target", ref);
        return false;
    }
    if (!CheckStateCode(ref)) {
        RecordFailure("%s: %p<%s> offset %zu targets %p<%s> in state %d outside ghost from-region", kind, holder,
                      (holder != nullptr) ? holder->GetTypeInfo()->GetName() : "root", offset, ref,
                      ref->GetTypeInfo()->GetName(), static_cast<int>(ref->GetStateWord().GetStateCode()));
        MaybeTraceRootPath("target", ref);
        return false;
    }
    // Target-liveness gate: a live holder (already passed the holder bitmap
    // gate in VerifyObject) must not reference a target in a garbage region
    // (waiting for FP reclaim) — that would mean a marking or corruption bug.
    // Resurrected objects are safe from this check: their regions are exempted
    // from the garbage list BEFORE the merge (ExemptFromRegions runs in
    // PostTrace, ahead of CollectFromSpaceGarbage), and IsSurvivedObject
    // covers the resurrectBitmap.
    RegionInfo::UnitRole role = RegionInfo::GetUnitRoleAt(reinterpret_cast<uintptr_t>(ref));
    if (role != RegionInfo::UnitRole::FREE_UNITS && role != RegionInfo::UnitRole::SUBORDINATE_UNIT) {
        RegionInfo* targetRegion = RegionInfo::GetLeaderRegionInfoAt(reinterpret_cast<uintptr_t>(ref));
        if (targetRegion != nullptr && (targetRegion->IsGarbageRegion() || targetRegion->IsFreeRegion())) {
            RecordFailure("%s: %p<%s> offset %zu targets %p<%s> in %s region (dangling reference to reclaimed object?)",
                          kind, holder,
                          (holder != nullptr) ? holder->GetTypeInfo()->GetName() : "root", offset, ref,
                          ref->GetTypeInfo()->GetName(),
                          targetRegion->IsGarbageRegion() ? "GARBAGE" : "FREE");
            MaybeTraceRootPath("target", ref);
            return false;
        }
    }
    return true;
}

bool HeapVerifier::CheckStateCode(BaseObject* obj)
{
    switch (obj->GetStateWord().GetStateCode()) {
        case ObjectState::NORMAL:
        case ObjectState::LOCKED:
            return true;
        case ObjectState::FORWARDING:
        case ObjectState::FORWARDED:
            // Legal only as forwarding residue inside a ghost from-region.
            return RegionInfo::InGhostFromRegion(obj);
        default:
            return false;
    }
}

void HeapVerifier::VerifyRoots()
{
    Heap& heap = Heap::GetHeap();
    // Static roots: RefField-based slots.
    RefFieldVisitor staticVisitor = [this](RefField<>& field) {
        ++rootCount_;
        CheckTarget("static-root", nullptr, 0, ResolveRef(field));
    };
    heap.VisitStaticRoots(staticVisitor);

    // ObjectRef-based roots may carry tag bits in the raw value (same storage
    // as FinalizerProcessor lists); wrap through RefField to strip them.
    auto refVisitor = [this](ObjectRef& ref) {
        ++rootCount_;
        RefField<> refField(reinterpret_cast<MAddress>(ref.object));
        CheckTarget("root", nullptr, 0, ResolveRef(refField));
    };
    RootVisitor rootVisitor = refVisitor;

    heap.VisitAllExportRoots(rootVisitor);                               // export roots
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rootVisitor); // concurrency roots
    heap.GetFinalizerProcessor().VisitGCRoots(rootVisitor);              // FP roots
}

void HeapVerifier::RecordFailure(const char* fmt, ...)
{
    ++failCount_;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LOG(RTLOG_ERROR, "%s: %s", tag_, buf);
}

// Diagnostic for F4 (non-module target): log the /proc/self/maps line whose
// [start, end) range contains addr, or state that addr is unmapped. Best
// effort — open/parse failures degrade to a single ERROR line and never
// disturb the verifier's own failure accounting (RecordFailure not used).
void HeapVerifier::DumpAddressMapping(void* addr)
{
    FILE* file = std::fopen("/proc/self/maps", "r");
    if (file == nullptr) {
        LOG(RTLOG_ERROR, "HeapVerifier: cannot open /proc/self/maps for %p", addr);
        return;
    }
    const size_t bufSize = 1024; // maps lines are < 512 bytes on supported platforms
    char buf[bufSize];
    const unsigned long target = static_cast<unsigned long>(reinterpret_cast<uintptr_t>(addr));
    while (std::fgets(buf, bufSize, file) != nullptr) {
        unsigned long start = 0;
        unsigned long end = 0;
        if (std::sscanf(buf, "%lx-%lx", &start, &end) != 2) {
            continue;
        }
        if (target >= start && target < end) {
            char* newLine = std::strchr(buf, '\n');
            if (newLine != nullptr) {
                *newLine = '\0';
            }
            LOG(RTLOG_ERROR, "HeapVerifier: %p in mapping: %s", addr, buf);
            std::fclose(file);
            return;
        }
    }
    std::fclose(file);
    LOG(RTLOG_ERROR, "HeapVerifier: %p is not in any mapping (unmapped hole)", addr);
}

// ---------------------------------------------------------------------------
// Root-path tracing (cjVerifyRootPath=1, off by default).
// ART precedent: Verification::FirstPathFromRootSet (art/runtime/gc/
// verification.cc:206) — BFS from the root set over the reachable object
// graph, each queue entry carrying the path string of how it was reached;
// visited-set dedup makes every object claim exactly one (shortest, in BFS
// order) path; the first dequeue of the target returns its path. Only
// trusted+live objects are expanded (same gates as VerifyObject), and ghost
// corpses are not followed (stale pre-forwarding refs). Cost is a full
// reachable-graph BFS, paid only on the failure path right before abort.
// ---------------------------------------------------------------------------
void HeapVerifier::MaybeTraceRootPath(const char* what, BaseObject* obj)
{
    if (!IsPathTraceEnabled() || obj == nullptr) {
        return;
    }
    pendingPathTraces_.emplace_back(what, obj);
}

void HeapVerifier::FlushPathTraces()
{
    if (!IsPathTraceEnabled() || pendingPathTraces_.empty()) {
        return;
    }
    std::set<BaseObject*> traced; // same object may be reported by several checks
    for (const auto& pending : pendingPathTraces_) {
        if (traced.insert(pending.second).second) {
            LOG(RTLOG_ERROR, "%s: root-path to %s %p: %s", tag_, pending.first, pending.second,
                FirstPathFromRootSet(pending.second).c_str());
        }
    }
    pendingPathTraces_.clear();
}

// BFS over the reachable object graph with parent-pointer backtracking:
// the queue holds node-pool indices; each node records (obj, parent, offset).
// A path string is only materialized when the target is dequeued, so memory
// stays O(nodes) instead of O(nodes x path-length) — the naive
// string-per-entry variant (ART's) is fine on small test heaps but explodes
// on million-object heaps. Expansion gates mirror VerifyObject: heap address,
// trusted header, marked (or resurrected), not a ghost corpse.
std::string HeapVerifier::FirstPathFromRootSet(BaseObject* target) const
{
    Heap& heap = Heap::GetHeap();
    struct Node {
        BaseObject* obj;
        int parent;
        size_t off;
    };
    std::vector<Node> nodes;
    std::unordered_set<BaseObject*> visited;
    std::deque<size_t> work;
    // Node budget: bounds BOTH time (work per node is O(fields)) and memory
    // (~64B/node total incl. the hash set) of the worst case (unreachable
    // target -> full reachable-set sweep) under STW. Diagnostic value is
    // concentrated in short paths, which always fit; an exhausted budget
    // itself is signal (target unreachable or extremely deep).
    static constexpr size_t kMaxTraceNodes = 250000;
    bool budgetExhausted = false;
    auto describe = [](BaseObject* obj) -> std::string {
        char buf[96];
        if (obj->IsValidObject()) {
            (void)snprintf(buf, sizeof(buf), "%p<%s>", static_cast<void*>(obj), obj->GetTypeInfo()->GetName());
        } else {
            (void)snprintf(buf, sizeof(buf), "%p<header-untrusted>", static_cast<void*>(obj));
        }
        return std::string(buf);
    };
    auto tryEnqueue = [&nodes, &visited, &work, &budgetExhausted](BaseObject* obj, int parent, size_t off) -> bool {
        // Heap-address gate FIRST: fields legally point at module constants
        // (.so data section) and may point at wild addresses — anything
        // outside the heap must never reach IsSurvivedObject/GetRegionInfoAt,
        // which abort on out-of-range addresses.
        if (obj == nullptr || !Heap::IsHeapAddress(reinterpret_cast<MAddress>(obj))) {
            return false;
        }
        if (!visited.insert(obj).second) {
            return false;
        }
        if (nodes.size() >= kMaxTraceNodes) {
            budgetExhausted = true;
            return false;
        }
        nodes.push_back({obj, parent, off});
        work.push_back(nodes.size() - 1);
        return true;
    };

    // Seed the queue from the same four root sources VerifyRoots uses.
    RefFieldVisitor staticVisitor = [this, &tryEnqueue](RefField<>& field) {
        tryEnqueue(ResolveRef(field), -1, 0);
    };
    heap.VisitStaticRoots(staticVisitor);
    auto refVisitor = [this, &tryEnqueue](ObjectRef& ref) {
        RefField<> refField(reinterpret_cast<MAddress>(ref.object));
        tryEnqueue(ResolveRef(refField), -1, 0);
    };
    RootVisitor rootVisitor = refVisitor;
    heap.VisitAllExportRoots(rootVisitor);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rootVisitor);
    heap.GetFinalizerProcessor().VisitGCRoots(rootVisitor);
    // Mutator stack/exception/native-frame roots — the same source the GC's
    // remark phase scans (TracingCollector.cpp:631, same STW context). Without
    // these, objects held only by local variables — the most common holder
    // form in business code — are invisible to the BFS and "<no path>" becomes
    // a misleading answer.
    MutatorManager::Instance().VisitAllMutators(
        [&rootVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(rootVisitor); });

    while (!work.empty()) {
        size_t idx = work.front();
        work.pop_front();
        Node& node = nodes[idx];
        if (node.obj == target) {
            // Backtrack the chain and materialize the path string once.
            std::vector<size_t> chain;
            for (int i = static_cast<int>(idx); i >= 0; i = nodes[i].parent) {
                chain.push_back(static_cast<size_t>(i));
            }
            std::reverse(chain.begin(), chain.end());
            std::string path = "root = " + describe(nodes[chain[0]].obj);
            for (size_t k = 1; k < chain.size(); ++k) {
                path += " -> +" + std::to_string(nodes[chain[k]].off) + " " + describe(nodes[chain[k]].obj);
            }
            return path;
        }
        BaseObject* cur = node.obj;
        // Tracing gate (looser than VerifyObject's on purpose): a trusted
        // header and not a ghost corpse. The mark-bitmap gate is deliberately
        // NOT applied here — forwarded to-space objects and TL-region bump
        // allocations are live but unmarked, and pruning them (the verifier's
        // conservative check semantics) would break every path crossing a
        // forwarded object. Worst case this walks a stale reference of
        // floating garbage: harmless for a read-only diagnostic, and the
        // node budget bounds the damage.
        if (!cur->IsValidObject() || RegionInfo::InGhostFromRegion(cur)) {
            continue;
        }
        if (!cur->GetTypeInfo()->HasRefField()) {
            continue;
        }
        cur->ForEachRefField([this, cur, idx, &tryEnqueue](RefField<>& field) {
            BaseObject* t = ResolveRef(field);
            if (t != nullptr) {
                size_t off = static_cast<size_t>(reinterpret_cast<MAddress>(&field) -
                                                 reinterpret_cast<MAddress>(cur));
                tryEnqueue(t, static_cast<int>(idx), off);
            }
        });
    }
    if (budgetExhausted) {
        return "<trace aborted: node budget of 250000 exceeded (target unreachable or path too deep)>";
    }
    return "<no path from root set>";
}
} // namespace MapleRuntime
