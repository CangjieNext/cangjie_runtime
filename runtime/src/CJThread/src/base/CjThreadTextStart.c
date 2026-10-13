// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// CJThread text range start marker.
//
// This object must be the FIRST member of libcangjie-thread.a (see
// CJThread/src/CMakeLists.txt). The Apple linker lays pulled archive members
// in archive member order, so every other thread archive member is placed
// after the symbol below. Its address forms the start of the CJThread text
// range consumed together with the runtime range by
// StackManager::IsRuntimeFrame (see os/Macos/Section.cpp).
//
// The symbol name is deliberately distinct from the runtime archive markers:
// with identical names, only the first defining archive member would ever be
// pulled and the second range would silently collapse.
//
// The body is never executed; the distinct constant avoids identical-code
// folding with the end marker.

__attribute__((used)) void _CJ_Thread_Text_Start(void)
{
    volatile int sink = 0x5A5B;
    (void)sink;
}
