// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RememberSet.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {

RememberSet& RememberSet::Instance() noexcept
{
    static RememberSet instance;
    return instance;
}

void RememberSet::Record(RegionInfo* target, uintptr_t sourceAddr)
{
    std::lock_guard<std::mutex> lock(mtx_);
    rememberedSlots[target].insert(sourceAddr);
}

void RememberSet::Clear(RegionInfo* target)
{
    std::lock_guard<std::mutex> lock(mtx_);
    rememberedSlots.erase(target);
}

void RememberSet::ClearAll()
{
    std::lock_guard<std::mutex> lock(mtx_);
    rememberedSlots.clear();
}

void RememberSet::MigrateGhostRegionSlots(RegionInfo* oldRegion)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = rememberedSlots.find(oldRegion);
    if (it == rememberedSlots.end()) {
        return;
    }
    auto slots = std::move(it->second);
    rememberedSlots.erase(it);
    // At this point, slot values have already been updated to forwarded
    // addresses by PromptRegionImpl. Read the new target and find its
    // region to migrate the slot into the correct region's slot set.
    for (uintptr_t slotAddr : slots) {
        RefField<>* field = reinterpret_cast<RefField<>*>(slotAddr);
        BaseObject* target = field->GetTargetObject();
        if (target == nullptr) {
            continue;
        }
        RegionInfo* newRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (newRegion != nullptr && newRegion != oldRegion) {
            rememberedSlots[newRegion].insert(slotAddr);
        }
    }
}

} // namespace MapleRuntime
