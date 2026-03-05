// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "MarkSweepCollector.h"
#include "Base/LogFile.h"

namespace MapleRuntime {

void MarkSweepCollector::Init()
{
    VLOG(REPORT, "[MS-GC] MarkSweepCollector::Init()");
    TracingCollector::Init();
}

void MarkSweepCollector::Fini()
{
    VLOG(REPORT, "[MS-GC] MarkSweepCollector::Fini()");
    TracingCollector::Fini();
}

void MarkSweepCollector::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
    VLOG(REPORT, "[MS-GC] RunGarbageCollection #%llu reason=%u", static_cast<unsigned long long>(gcIndex), reason);
    SetGCReason(reason);
    PreGarbageCollection(/* isConcurrent */ true);
    DoGarbageCollection(gcIndex);
    PostGarbageCollection(gcIndex);
}

void MarkSweepCollector::DoGarbageCollection(uint64_t gcIndex)
{
    VLOG(REPORT, "[MS-GC] DoGarbageCollection #%llu — skeleton (no-op)", static_cast<unsigned long long>(gcIndex));
    // Step 1+ will fill in:
    // MarkSetup();
    // ConcurrentMark();
    // MarkTermination();
    // ConcurrentSweep();
}

void MarkSweepCollector::MarkSetup()
{
    // TODO(Step 1): STW — enable write barrier, scan GC roots
}

void MarkSweepCollector::ConcurrentMark()
{
    // TODO(Step 2): Concurrent tri-color marking
}

void MarkSweepCollector::MarkTermination()
{
    // TODO(Step 2): STW — drain SATB buffer, finish marking
}

void MarkSweepCollector::ConcurrentSweep()
{
    // TODO(Step 3): Concurrent sweep of dead objects
}

// ── TracingCollector virtual overrides ──

void MarkSweepCollector::EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const
{
    BaseObject* target = ref.GetTargetObject();
    if (target != nullptr) {
        rootSet.push_back(target);
    }
}

void MarkSweepCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack)
{
    // TODO(Step 2): Scan all reference fields of obj, push unmarked refs into workStack
    (void)obj;
    (void)workStack;
}

BaseObject* MarkSweepCollector::GetAndTryTagObj(BaseObject* obj, RefField<>& field)
{
    // No tagging in Mark-Sweep — return object as-is
    (void)field;
    return obj;
}

RefField<> MarkSweepCollector::GetAndTryTagRefField(BaseObject* obj) const
{
    // No tagging in Mark-Sweep — return a RefField wrapping the object directly
    return RefField<>(obj);
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
