// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "SatbBuffer.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"

#include "Base/ImmortalWrapper.h"

namespace MapleRuntime {
static ImmortalWrapper<SatbBuffer> g_instance;

SatbBuffer& SatbBuffer::Instance() noexcept { return *g_instance; }

bool SatbBuffer::ShouldEnqueue(const BaseObject* obj)
{
    if (!Heap::GetHeap().GetCollector().IsTargetAddress(obj)) {
        return false;
    }
    return RegionSpace::ShouldEnqueue(obj);
}

static ImmortalWrapper<WeakRefBuffer> g_weakRefBuffer;

WeakRefBuffer& WeakRefBuffer::Instance() noexcept { return *g_weakRefBuffer; }

BaseObject* WeakRefBuffer::ReadWeakRefInPostTrace(
    BaseObject* obj, RefField<false>& field, const Collector& collector)
{
    // Serialize with ClearWeakRefBuffer so a referent cannot be reclaimed between
    // checking the current-GC weak-ref set and checking its mark bit.
    std::lock_guard<std::mutex> lock(mtx);

    auto& atomicField = reinterpret_cast<RefField<true>&>(field);
    RefField<> current(atomicField.GetFieldValue());
    if (current.IsTagged()) {
        CHECK(collector.IsCurrentPointer(current));
    }
    BaseObject* referent = current.GetTargetObject();
    if (referent == nullptr) {
        return nullptr;
    }

    // An absent wrapper either did not need weak processing this cycle (new
    // allocations or a conservatively retained resurrection graph), or has
    // already been processed below.
    // New wrappers cannot introduce a dead referent: construction requires a
    // strong reference, and Enum/Trace weak reads protect theirs through SATB.
    // Processed wrappers have already had dead referents cleared. Do not consult
    // their marks again: PostTrace may subsequently reset a live large object's
    // mark. In a young GC, non-target referents must also be preserved.
    bool participatesInCurrentGC = refObjBuffer.find(obj) != refObjBuffer.end() &&
        collector.IsTargetAddress(referent);
    if (!participatesInCurrentGC) {
        return referent;
    }

    RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(referent));
    return regionInfo->IsMarkedObject(referent) ? referent : nullptr;
}

void WeakRefBuffer::ClearWeakRefBuffer(const Collector& collector)
{
    // No tracer can insert after the phase handshake. Retire one wrapper per
    // critical section so readers need not wait for the entire weak-ref list.
    // Field inspection/clearing and membership removal must remain indivisible.
    for (;;) {
        std::lock_guard<std::mutex> lock(mtx);
        if (refObjBuffer.empty()) {
            return;
        }
        auto it = refObjBuffer.begin();
        BaseObject* obj = *it;
        auto* referentField = reinterpret_cast<RefField<true>*>(reinterpret_cast<uintptr_t>(obj) + TYPEINFO_PTR_SIZE);
        RefField<> oldField(referentField->GetFieldValue());
        BaseObject* referent = oldField.GetTargetObject();
        if (referent != nullptr && collector.IsTargetAddress(referent)) {
            RegionInfo* regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(referent));
            if (!regionInfo->IsMarkedObject(referent)) {
                bool cleared = referentField->CompareExchange(oldField.GetFieldValue(), 0);
                // The only concurrent update permitted by WeakRef's API is
                // clear(). Never retire a dead referent if it was rebound.
                CHECK_DETAIL(cleared || referentField->GetFieldValue() == 0,
                             "WeakRef %p referent changed during PostTrace clearing", obj);
            }
        }
        refObjBuffer.erase(it);
    }
}
} // namespace MapleRuntime
