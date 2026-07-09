// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_ASSIST_CONFIG_H
#define MRT_MS_ASSIST_CONFIG_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstdint>

#include "Cangjie.h"

namespace MapleRuntime {

constexpr uint8_t kMSAssistSwitchDefault = 0U;
constexpr uint8_t kMSAssistSwitchDisabled = 1U;
constexpr uint8_t kMSAssistSwitchEnabled = 2U;

inline uint8_t SanitizeMSAssistSwitchValue(uint8_t value)
{
    switch (value) {
        case kMSAssistSwitchDefault:
        case kMSAssistSwitchDisabled:
        case kMSAssistSwitchEnabled:
            return value;
        default:
            return kMSAssistSwitchDefault;
    }
}

inline uint8_t ResolveMSAssistSwitch(uint8_t value)
{
    return SanitizeMSAssistSwitchValue(value) == kMSAssistSwitchDisabled ? 0U : 1U;
}

inline bool IsMSEnumAssistEnabled(const GCParam& param)
{
    return ResolveMSAssistSwitch(param.msEnumAssist) != 0;
}

} // namespace MapleRuntime

#endif  // MRT_USE_MARK_SWEEP
#endif  // MRT_MS_ASSIST_CONFIG_H
