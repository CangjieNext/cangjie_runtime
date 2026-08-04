// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REMEMBER_SET_H
#define MRT_REMEMBER_SET_H

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Allocator/RegionInfo.h"

namespace MapleRuntime {

class RememberSet {
public:
    static RememberSet& Instance() noexcept;

    void Record(RegionInfo* target, uintptr_t sourceAddr);

    template <typename Func>
    void VisitRememberedSlots(RegionInfo* target, Func&& func)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = rememberedSlots.find(target);
        if (it != rememberedSlots.end()) {
            for (uintptr_t slotAddr : it->second) {
                func(slotAddr);
            }
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
            for (uintptr_t slotAddr : pair.second) {
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

    std::unordered_map<RegionInfo*, std::unordered_set<uintptr_t>> rememberedSlots;
    std::mutex mtx_;
};

} // namespace MapleRuntime

#endif // MRT_REMEMBER_SET_H
