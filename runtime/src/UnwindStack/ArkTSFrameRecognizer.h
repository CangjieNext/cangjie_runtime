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
    static bool IsArkTSFrameByPc(uintptr_t pc);
    static bool IsInStubRange(uintptr_t pc);
    static bool IsInInterpreterRange(uintptr_t pc);
    static bool ExtractFrameInfo(FrameAddress* fa, ArkTSFrameInfo& info);
    static bool IsArkTSRuntimeAvailable();

private:
    static bool InitArkRanges();
    static bool InitArkStubRange();
    static bool InitArkInterpreterRange();
    static FrameAddress* GetPrevFp(FrameAddress* fa);
    static const uint32_t* GetReturnAddr(FrameAddress* fa);
    
    static uintptr_t arkStubBegin_;
    static uintptr_t arkStubEnd_;
    static uintptr_t arkInterpreterBegin_;
    static uintptr_t arkInterpreterEnd_;
    static bool arkRangeInitialized_;
};

} // namespace MapleRuntime

#endif // MRT_ARKTS_FRAME_RECOGNIZER_H