// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "HeapVerifier.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

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
        return;
    }
    if (!obj->IsValidObject()) {
        RecordFailure("object %p has invalid header (null or corrupted TypeInfo)", obj);
        return; // header untrusted: skip ref-field traversal (spec 6.1)
    }
    if (!CheckStateCode(obj)) {
        RecordFailure("object %p<%s> has unexpected state %d outside ghost from-region", obj,
                      obj->GetTypeInfo()->GetName(), static_cast<int>(obj->GetStateWord().GetStateCode()));
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
BaseObject* HeapVerifier::ResolveRef(RefField<>& field)
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
        return true; // legal non-heap target (e.g. constant strings), collector does the same
    }
    // Allocator::IsHeapObject is only declared under MRT_DEBUG (Allocator.h);
    // every in-tree caller (e.g. TracingCollector::DumpRoots) uses the same guard.
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    if (!Heap::GetHeap().GetAllocator().IsHeapObject(reinterpret_cast<MAddress>(ref))) {
        RecordFailure("%s: %p<%s> offset %zu targets %p which is not a heap object", kind, holder,
                      (holder != nullptr) ? holder->GetTypeInfo()->GetName() : "root", offset, ref);
        return false;
    }
#endif
    if (!ref->IsValidObject()) {
        RecordFailure("%s: %p<%s> offset %zu targets %p with invalid header", kind, holder,
                      (holder != nullptr) ? holder->GetTypeInfo()->GetName() : "root", offset, ref);
        return false;
    }
    if (!CheckStateCode(ref)) {
        RecordFailure("%s: %p<%s> offset %zu targets %p<%s> in state %d outside ghost from-region", kind, holder,
                      (holder != nullptr) ? holder->GetTypeInfo()->GetName() : "root", offset, ref,
                      ref->GetTypeInfo()->GetName(), static_cast<int>(ref->GetStateWord().GetStateCode()));
        return false;
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
} // namespace MapleRuntime
