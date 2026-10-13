// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Runtime text range start marker.
//
// This object must be the FIRST member of libcangjie-runtime.a (see
// runtime/CMakeLists.txt). The Apple linker lays pulled archive members in
// archive member order, so every other runtime archive member is placed after
// the symbol below. Its address forms the start of the runtime text range
// used by StackManager::IsRuntimeFrame (see os/Macos/Section.cpp).
//
// The body is never executed; the distinct constant avoids identical-code
// folding with the end marker.

__attribute__((used)) void _CJ_Text_Start(void)
{
    volatile int sink = 0x5A5A;
    (void)sink;
}
