// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information

#include "RememberSet.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {

RememberSet& RememberSet::Instance() noexcept
{
    static RememberSet instance;
    return instance;
}

void RememberSet::Record(RegionInfo* target, uintptr_t slotAddr)
{
    RegionInfo* src = RegionInfo::GetRegionInfoAt(slotAddr);
    if (src == nullptr) {
        return;
    }
    size_t slotIdx = (slotAddr - src->GetRegionStart()) / RegionInfo::RSET_SLOT_SIZE;
    std::lock_guard<std::mutex> lock(mtx_);
    src->MarkRSetSlot(slotIdx);
    rememberedSlots[target].insert({src, slotIdx});
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
    auto entries = std::move(it->second);
    rememberedSlots.erase(it);
    // At this point, slot values have already been updated to forwarded
    // addresses by PromptRegionImpl. Read the new target and find its
    // region to migrate the slot into the correct region's slot list.
    for (auto& entry : entries) {
        RegionInfo* src = entry.first;
        size_t slotIdx = entry.second;
        uintptr_t slotAddr = src->GetRegionStart() + slotIdx * RegionInfo::RSET_SLOT_SIZE;
        RefField<>* field = reinterpret_cast<RefField<>*>(slotAddr);
        BaseObject* target = field->GetTargetObject();
        if (target == nullptr) {
            continue;
        }
        RegionInfo* newRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (newRegion != nullptr && newRegion != oldRegion) {
            rememberedSlots[newRegion].insert({src, slotIdx});
        }
    }
}

void RememberSet::MergeFrom(RSetLocalSlots&& local)
{
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& [target, slots] : local) {
        auto& globalSlots = rememberedSlots[target];
        for (auto& entry : slots) {
            globalSlots.insert(entry);
        }
    }
}

} // namespace MapleRuntime
