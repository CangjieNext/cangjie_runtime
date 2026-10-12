// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_VERIFY_ROOT_CONTEXT_H
#define MRT_VERIFY_ROOT_CONTEXT_H

#include <cstdint>

namespace MapleRuntime {
class Mutator;

// Borrowed only during one verification callback. No object or frame history.
struct VerifyRootContext {
    enum class Location { NONE, STACK_SLOT, REGISTER };

    const Mutator* mutator = nullptr;
    uintptr_t pc = 0;
    uintptr_t frame = 0;
    Location location = Location::NONE;
    int32_t slotOffset = 0;
    uint32_t reg = 0;

    void ClearFrame()
    {
        pc = frame = 0;
        location = Location::NONE;
    }
};
} // namespace MapleRuntime

#endif // MRT_VERIFY_ROOT_CONTEXT_H
