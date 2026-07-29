// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_SCAN_CLASS_H
#define MRT_MS_SCAN_CLASS_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

class TypeInfo;

enum class MSScanClass : uint8_t {
    NO_REF = 0,
    HAS_REF = 1,
};

constexpr size_t kMSScanClassCount = 2;

inline constexpr uint8_t ToMSScanClassIndex(MSScanClass scanClass)
{
    return static_cast<uint8_t>(scanClass);
}

MSScanClass ClassifyMSScanClass(const TypeInfo* typeInfo);

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_SCAN_CLASS_H
