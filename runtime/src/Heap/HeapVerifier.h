// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_VERIFIER_H
#define MRT_HEAP_VERIFIER_H

#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include <string>

namespace MapleRuntime {

// GC pre/post-cycle heap verification (walker variant, ADR-12).
// Spec: docs/superpowers/specs/2026-09-10-heap-verify-walker-design.md
// Enabled via cjPreVerify / cjPostVerify ("1"/"true"/"on", case-insensitive).
// Read-only: reports and fatals on failure, never mutates the heap.
//
// The walker navigates the heap without reading any field in FP's write set
// (non-0-suffix metadata of garbage/free units), eliminating the need for the
// FP handshake. Ghost regions are crossed via frozen 0-suffix extents; free
// and subordinate units are crawled one unit at a time (never reading
// ownerRegion); leader-role units read extent+type, which is safe because
// InitRegionInfo writes regionEnd before SetUnitRole.
class HeapVerifier {
public:
    // FP activity counters snapshotted at GC cycle start; used to attribute
    // verification failures (ADR-11 section 8.2, retained per ADR-12 section 7).
    struct FpSnapshot {
        uint64_t finalizedCount = 0;
        uint64_t enqueuedCount = 0;
    };

    static bool IsPreVerifyEnabled();
    static bool IsPostVerifyEnabled();

    HeapVerifier(const char* tag, const FpSnapshot& fpSnapshot) : tag_(tag), fpSnapshot_(fpSnapshot) {}
    ~HeapVerifier() = default;

    // Walks all heap objects and root sets (ADR-12 section 10).
    // Requires STW held by the caller (mutators/finalizers frozen; FP's
    // metadata jobs run freely — the walker is immune to them by design).
    void Verify();

private:
    // Walker: navigates the heap from regionHeapStart to inactiveZone using
    // the ghost-0-suffix + crawl rules (spec section 3). Never reads
    // ownerRegion (non-0), never reads non-0 fields of ghost regions, and
    // only touches FP-written fields through the write-order-safe leader path.
    void WalkHeap();

    void VerifyObject(BaseObject* obj);
    void VerifyReference(BaseObject* holder, RefField<>& field);
    // Resolve a ref-field target the way the runtime reads it between GC cycles
    // (read-only IdleBarrier semantics): untagged -> raw; tagged -> forward-table
    // lookup with raw-address fallback when unroutable. Never writes/re-tags.
    BaseObject* ResolveRef(RefField<>& field) const;
    // Core one-hop target check shared by ref-fields and roots (ADR-12 section 10.1).
    // Returns true when ok or legally skipped; false when a failure was recorded.
    bool CheckTarget(const char* kind, BaseObject* holder, size_t offset, BaseObject* ref);
    // State code rule (ADR-12 section 10.1, F3/F6): NORMAL/LOCKED always legal;
    // FORWARDING/FORWARDED only inside ghost from-regions.
    static bool CheckStateCode(BaseObject* obj);
    void VerifyRoots();
    void RecordFailure(const char* fmt, ...);
    // Diagnostic: log the /proc/self/maps line containing the given address
    // (used to triage non-module targets reported by CheckTarget).
    static void DumpAddressMapping(void* addr);
    // Diagnostic (cjVerifyRootPath=1, off by default): on a verification
    // failure, additionally trace and log the first shortest reference path
    // from the root set to the offending object (ART precedent:
    // Verification::FirstPathFromRootSet, verification.cc:206). Full
    // reachable-graph BFS — only paid on the failure path, where the verifier
    // aborts right after, so the O(heap) cost is acceptable.
    static bool IsPathTraceEnabled();
    // F4 (non-heap target examination) is opt-in via cjVerifyNonHeap
    // ("1"/"true"/"on", case-insensitive, default off): report ref-fields and
    // roots whose target is a non-heap, non-module address as wild pointers.
    // Legal passes: module constants (dladdr) and registered native roots
    // (futureSet) on every platform; CJThread stack captures additionally on
    // stack-growth-free platforms (OHOS/ANDROID, complete allCJThreadList
    // oracle). Default off tolerates non-heap targets exactly like the
    // collector does (TraceRefField give-up).
    static bool IsNonHeapVerifyEnabled();
    // Failure-path hook: records (what, obj) for deferred tracing. Deferred on
    // purpose — the failure may fire inside a Visit*Roots callback, and the
    // BFS itself iterates the same root sources; re-entering that iteration
    // from within its own callback deadlocks. FlushPathTraces runs after both
    // WalkHeap and VerifyRoots have returned.
    void MaybeTraceRootPath(const char* what, BaseObject* obj);
    void FlushPathTraces();
    std::string FirstPathFromRootSet(BaseObject* target) const;

    const char* const tag_;
    const FpSnapshot fpSnapshot_;
    std::vector<std::pair<const char*, BaseObject*>> pendingPathTraces_;
    size_t failCount_ = 0;
    size_t objCount_ = 0;
    size_t rootCount_ = 0;
    uint64_t startTime_ = 0;
};
} // namespace MapleRuntime
#endif // MRT_HEAP_VERIFIER_H
