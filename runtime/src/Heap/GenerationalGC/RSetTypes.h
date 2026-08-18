// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RSET_TYPES_H
#define MRT_RSET_TYPES_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace MapleRuntime {

class RegionInfo;

struct RSetSlotHash {
    size_t operator()(const std::pair<RegionInfo*, size_t>& p) const noexcept
    {
        size_t h1 = std::hash<RegionInfo*>{}(p.first);
        size_t h2 = std::hash<size_t>{}(p.second);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

using RSetLocalSlots = std::unordered_map<RegionInfo*, std::unordered_set<std::pair<RegionInfo*, size_t>, RSetSlotHash>>;

} // namespace MapleRuntime

#endif // MRT_RSET_TYPES_H
