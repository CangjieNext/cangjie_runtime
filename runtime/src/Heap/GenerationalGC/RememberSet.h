// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information

#ifndef MRT_REMEMBER_SET_H
#define MRT_REMEMBER_SET_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Allocator/RegionInfo.h"

namespace MapleRuntime {

// RememberSet is a multi-level table recording cross-generational references.
// Top level (key): the young target region that is pointed into.
// Value: a list of (source old region, slot index) pairs. The slot index is
// (slotAddr - source.GetRegionStart()) / RSET_SLOT_SIZE, i.e. an exact pointer-
// sized slot within the source region. Each old region carries a per-region
// card table (RegionInfo::rsetCardTable, 1 bit/slot) that is reset when the
// region is freed by InitFreeRegion; during visit, entries whose source card
// table bit has been reset are skipped lazily.
class RememberSet {
public:
    static RememberSet& Instance() noexcept;

    void Record(RegionInfo* target, uintptr_t slotAddr);

    template <typename Func>
    void VisitRememberedSlots(RegionInfo* target, Func&& func)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = rememberedSlots.find(target);
        if (it == rememberedSlots.end()) {
            return;
        }
        for (auto& entry : it->second) {
            RegionInfo* src = entry.first;
            size_t slotIdx = entry.second;
            if (!src->IsRSetSlotMarked(slotIdx)) {
                continue;
            }
            uintptr_t slotAddr = src->GetRegionStart() + slotIdx * RegionInfo::RSET_SLOT_SIZE;
            func(slotAddr);
        }
    }

    template <typename Func>
    void VisitAllYoungSlots(Func&& func)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& pair : rememberedSlots) {
            if (pair.first->IsOldRegion()) {
                continue;
            }
            for (auto& entry : pair.second) {
                RegionInfo* src = entry.first;
                size_t slotIdx = entry.second;
                if (!src->IsRSetSlotMarked(slotIdx)) {
                    continue;
                }
                uintptr_t slotAddr = src->GetRegionStart() + slotIdx * RegionInfo::RSET_SLOT_SIZE;
                func(slotAddr);
            }
        }
    }

    void Clear(RegionInfo* target);
    void ClearAll();
    void MigrateGhostRegionSlots(RegionInfo* oldRegion);

    RememberSet(const RememberSet&) = delete;
    RememberSet& operator=(const RememberSet&) = delete;

private:
    RememberSet() = default;
    ~RememberSet() = default;

    std::unordered_map<RegionInfo*, std::vector<std::pair<RegionInfo*, size_t>>> rememberedSlots;
    std::mutex mtx_;
};

} // namespace MapleRuntime

#endif // MRT_REMEMBER_SET_H
