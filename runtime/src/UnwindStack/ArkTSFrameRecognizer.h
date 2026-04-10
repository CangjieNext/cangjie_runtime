// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ARKTS_FRAME_RECOGNIZER_H
#define MRT_ARKTS_FRAME_RECOGNIZER_H

#include "Common/StackType.h"

namespace MapleRuntime {

constexpr uint64_t ARKTS_FRAME_MARKER = 0xFFFFFFFFFFFFFFFFULL;

struct ArkTSFrameInfo {
    FrameAddress* framePtr;
    const uint32_t* returnAddr;
    CString functionName;
    CString fileName;
    uint32_t lineNumber;
    uint32_t columnNumber;
};

class ArkTSFrameRecognizer {
public:
    static bool IsArkTSFrame(const MachineFrame& frame);
    static bool IsArkTSFrame(FrameAddress* fa);
    static bool ExtractFrameInfo(FrameAddress* fa, ArkTSFrameInfo& info);
    static bool IsArkTSRuntimeAvailable();

private:
    static constexpr uintptr_t ARKTS_FRAME_TYPE_OFFSET = sizeof(uintptr_t);
    static bool IsValidArkTSFrameType(uintptr_t type);
    static FrameAddress* GetPrevFp(FrameAddress* fa);
    static const uint32_t* GetReturnAddr(FrameAddress* fa);
};

} // namespace MapleRuntime

#endif // MRT_ARKTS_FRAME_RECOGNIZER_H
