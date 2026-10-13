// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// CJThread text range end marker.
//
// This object must be the LAST member of libcangjie-thread.a (see
// CJThread/src/CMakeLists.txt). The Apple linker lays pulled archive members
// in archive member order, so every other thread archive member is placed
// before the symbol below. Its address forms the end of the CJThread text
// range consumed together with the runtime range by
// StackManager::IsRuntimeFrame (see os/Macos/Section.cpp).
//
// The body is never executed; the distinct constant avoids identical-code
// folding with the start marker.

__attribute__((used)) void _CJ_Thread_Text_End(void)
{
    volatile int sink = 0xA5A6;
    (void)sink;
}
