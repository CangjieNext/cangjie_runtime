// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_OOM_EMERGENCY_POLICY_H
#define MRT_MS_OOM_EMERGENCY_POLICY_H

#ifdef MRT_USE_MARK_SWEEP

#include <cstddef>
#include <cstdint>
#include <utility>

namespace MapleRuntime {

template<typename TryAllocate, typename EmergencyRecovery>
uintptr_t TryMSAllocationWithEmergencyRetry(bool allowEmergency,
                                            TryAllocate&& tryAllocate,
                                            EmergencyRecovery&& emergencyRecovery)
{
    uintptr_t addr = std::forward<TryAllocate>(tryAllocate)();
    if (addr != 0 || !allowEmergency) {
        return addr;
    }

    std::forward<EmergencyRecovery>(emergencyRecovery)();
    return std::forward<TryAllocate>(tryAllocate)();
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP

#endif // MRT_MS_OOM_EMERGENCY_POLICY_H
