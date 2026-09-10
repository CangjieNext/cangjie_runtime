// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_VERIFIER_H
#define MRT_HEAP_VERIFIER_H

#include "Common/BaseObject.h"
#include "Common/StateWord.h"

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
    // verification failures (spec section 8.2).
    struct FpSnapshot {
        uint64_t finalizedCount = 0;
        uint64_t enqueuedCount = 0;
    };

    static bool IsPreVerifyEnabled();
    static bool IsPostVerifyEnabled();

    HeapVerifier(const char* tag, const FpSnapshot& fpSnapshot) : tag_(tag), fpSnapshot_(fpSnapshot) {}
    ~HeapVerifier() = default;

    // Walks all heap objects and root sets (spec section 4.3).
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
    BaseObject* ResolveRef(RefField<>& field);
    // Core one-hop target check shared by ref-fields and roots (spec section 6.2).
    // Returns true when ok or legally skipped; false when a failure was recorded.
    bool CheckTarget(const char* kind, BaseObject* holder, size_t offset, BaseObject* ref);
    // State code rule (spec section 6.3): NORMAL/LOCKED always legal;
    // FORWARDING/FORWARDED only inside ghost from-regions.
    static bool CheckStateCode(BaseObject* obj);
    void VerifyRoots();
    void RecordFailure(const char* fmt, ...);

    const char* const tag_;
    const FpSnapshot fpSnapshot_;
    size_t failCount_ = 0;
    size_t objCount_ = 0;
    size_t rootCount_ = 0;
    uint64_t startTime_ = 0;
};
} // namespace MapleRuntime
#endif // MRT_HEAP_VERIFIER_H
