// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ENV_PARAM_H
#define MRT_ENV_PARAM_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class CString;

enum class EnvBoundKind : uint8_t {
    OPEN,
    CLOSED,
};

bool TryReadEnv(const char* name, CString& value);
bool ReadBoolParameter(const char* name, bool defaultValue);
size_t ReadCountParameter(const char* name, size_t defaultValue, size_t minValue, EnvBoundKind minBound,
                          size_t maxValue = static_cast<size_t>(-1),
                          EnvBoundKind maxBound = EnvBoundKind::CLOSED);
size_t ReadSizeParameter(const char* name, size_t defaultValue, size_t minValue, EnvBoundKind minBound,
                         size_t maxValue = static_cast<size_t>(-1),
                         EnvBoundKind maxBound = EnvBoundKind::CLOSED);
uint64_t ReadTimeParameter(const char* name, uint64_t defaultValue, uint64_t minValue, EnvBoundKind minBound,
                           bool allowZero = false);
double ReadDoubleParameter(const char* name, double defaultValue, double minValue, EnvBoundKind minBound,
                           double maxValue, EnvBoundKind maxBound);
} // namespace MapleRuntime

#endif // MRT_ENV_PARAM_H
