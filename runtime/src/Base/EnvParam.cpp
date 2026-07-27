// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Base/EnvParam.h"

#include <cstdlib>
#include <limits>

#include "Base/CString.h"
#include "Base/Log.h"

namespace MapleRuntime {
namespace {
template <typename T>
bool CheckLowerBound(T value, T minValue, EnvBoundKind minBound)
{
    return minBound == EnvBoundKind::CLOSED ? value >= minValue : value > minValue;
}

template <typename T>
bool CheckUpperBound(T value, T maxValue, EnvBoundKind maxBound)
{
    return maxBound == EnvBoundKind::CLOSED ? value <= maxValue : value < maxValue;
}

const char* BoundText(EnvBoundKind bound, bool lower)
{
    if (lower) {
        return bound == EnvBoundKind::CLOSED ? ">=" : ">";
    }
    return bound == EnvBoundKind::CLOSED ? "<=" : "<";
}

bool EqualsIgnoreCase(const CString& lhs, const char* rhs)
{
    if (lhs.Str() == nullptr || rhs == nullptr) {
        return false;
    }
    const char* l = lhs.Str();
    while (*l != '\0' && *rhs != '\0') {
        char lc = *l;
        char rc = *rhs;
        if (lc >= 'A' && lc <= 'Z') {
            lc = static_cast<char>(lc - 'A' + 'a');
        }
        if (rc >= 'A' && rc <= 'Z') {
            rc = static_cast<char>(rc - 'A' + 'a');
        }
        if (lc != rc) {
            return false;
        }
        ++l;
        ++rhs;
    }
    return *l == '\0' && *rhs == '\0';
}
} // namespace

bool TryReadEnv(const char* name, CString& value)
{
    const char* env = std::getenv(name);
    if (env == nullptr || env[0] == '\0') {
        return false;
    }
    value = CString(env).RemoveBlankSpace();
    return value.Str() != nullptr && !value.IsEmpty();
}

bool ReadBoolParameter(const char* name, bool defaultValue)
{
    CString value;
    if (!TryReadEnv(name, value)) {
        return defaultValue;
    }

    if (value == "1" || EqualsIgnoreCase(value, "true") || EqualsIgnoreCase(value, "on") ||
        EqualsIgnoreCase(value, "yes")) {
        return true;
    }
    if (value == "0" || EqualsIgnoreCase(value, "false") || EqualsIgnoreCase(value, "off") ||
        EqualsIgnoreCase(value, "no")) {
        return false;
    }

    LOG(RTLOG_ERROR, "Unsupported %s parameter. Valid values are 0/1/true/false/on/off/yes/no.\n", name);
    return defaultValue;
}

size_t ReadCountParameter(const char* name, size_t defaultValue, size_t minValue, EnvBoundKind minBound,
                          size_t maxValue, EnvBoundKind maxBound)
{
    CString value;
    if (!TryReadEnv(name, value)) {
        return defaultValue;
    }

    size_t parsed = CString::ParsePosNumFromEnv(value);
    bool parsedZero = value == "0";
    if ((parsed != 0 || parsedZero) && CheckLowerBound(parsed, minValue, minBound) &&
        CheckUpperBound(parsed, maxValue, maxBound)) {
        return parsed;
    }

    LOG(RTLOG_ERROR, "Unsupported %s parameter. Valid range is %s %zu and %s %zu.\n",
        name, BoundText(minBound, true), minValue, BoundText(maxBound, false), maxValue);
    return defaultValue;
}

size_t ReadSizeParameter(const char* name, size_t defaultValue, size_t minValue, EnvBoundKind minBound,
                         size_t maxValue, EnvBoundKind maxBound)
{
    CString value;
    if (!TryReadEnv(name, value)) {
        return defaultValue;
    }

    size_t parsed = CString::ParseSizeFromEnv(value);
    if (parsed != 0 && CheckLowerBound(parsed, minValue, minBound) && CheckUpperBound(parsed, maxValue, maxBound)) {
        return parsed;
    }

    LOG(RTLOG_ERROR, "Unsupported %s parameter. Valid range is %s %zuKB and %s %zuKB.\n",
        name, BoundText(minBound, true), minValue, BoundText(maxBound, false), maxValue);
    return defaultValue;
}

uint64_t ReadTimeParameter(const char* name, uint64_t defaultValue, uint64_t minValue, EnvBoundKind minBound,
                           bool allowZero)
{
    CString value;
    if (!TryReadEnv(name, value)) {
        return defaultValue;
    }

    if (allowZero && value == "0") {
        return 0;
    }

    uint64_t parsed = CString::ParseTimeFromEnv(value);
    if (parsed != 0 && CheckLowerBound(parsed, minValue, minBound)) {
        return parsed;
    }

    LOG(RTLOG_ERROR, "Unsupported %s parameter. Valid range is %s%s %llu ns.\n",
        name, allowZero ? "0 or " : "", BoundText(minBound, true), static_cast<unsigned long long>(minValue));
    return defaultValue;
}

double ReadDoubleParameter(const char* name, double defaultValue, double minValue, EnvBoundKind minBound,
                           double maxValue, EnvBoundKind maxBound)
{
    CString value;
    if (!TryReadEnv(name, value)) {
        return defaultValue;
    }

    double parsed = CString::ParsePosDecFromEnv(value);
    if (parsed != 0.0 && CheckLowerBound(parsed, minValue, minBound) && CheckUpperBound(parsed, maxValue, maxBound)) {
        return parsed;
    }

    LOG(RTLOG_ERROR, "Unsupported %s parameter. Valid range is %s %f and %s %f.\n",
        name, BoundText(minBound, true), minValue, BoundText(maxBound, false), maxValue);
    return defaultValue;
}
} // namespace MapleRuntime
