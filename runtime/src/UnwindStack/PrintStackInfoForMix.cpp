// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "PrintStackInfoForMix.h"

#include <cstdint>
#include <dlfcn.h>

#include "ArkTSFrameRecognizer.h"
#include "Base/Log.h"
#include "Base/LogFile.h"

namespace MapleRuntime {

void PrintStackInfoForMix::FillInStackTrace()
{
    UnwindContext uwContext;
    CheckTopUnwindContextAndInit(uwContext);
    
    anchorFA = nullptr;
    
    bool arkTSRuntimeAvailable = ArkTSFrameRecognizer::IsArkTSRuntimeAvailable();
    
    while (!ShouldStopUnwind(uwContext.frameInfo.mFrame)) {
        AnalyseAndSetFrameType(uwContext);
        FrameType currentType = uwContext.frameInfo.GetFrameType();
        
        if (currentType == FrameType::MANAGED) {
            stack.emplace_back(uwContext.frameInfo);
        } else if (currentType == FrameType::ARKTS_FRAME && arkTSRuntimeAvailable) {
            stack.emplace_back(uwContext.frameInfo);
        }
        
        if (currentType == FrameType::ARKTS_FRAME && arkTSRuntimeAvailable) {
            if (!UnwindToCallerArkTSFrame(uwContext)) {
                break;
            }
        } else {
            UnwindContext caller;
            lastFrameType = currentType;
#ifndef _WIN64
            if (uwContext.frameInfo.mFrame.UnwindToCallerMachineFrame(caller.frameInfo.mFrame) == false) {
#else
            if (uwContext.frameInfo.mFrame.UnwindToCallerMachineFrame(caller.frameInfo.mFrame, uwCtxStatus) == false) {
#endif
                break;
            }
            uwContext.frameInfo = caller.frameInfo;
        }
    }
}

bool PrintStackInfoForMix::UnwindToCallerArkTSFrame(UnwindContext& uwContext)
{
    static bool arkTSUnwinderInitialized = false;
    static StepArkFunc gStepArk = nullptr;
    
    if (!arkTSUnwinderInitialized) {
        gStepArk = LoadArkTSStepArk();
        arkTSUnwinderInitialized = true;
    }
    
    if (gStepArk == nullptr) {
        UnwindContext caller;
        if (uwContext.frameInfo.mFrame.UnwindToCallerMachineFrame(caller.frameInfo.mFrame) == false) {
            return false;
        }
        uwContext.frameInfo = caller.frameInfo;
        return true;
    }
    
    ArkStepParam arkParam;
    FrameAddress* fa = const_cast<FrameAddress*>(uwContext.frameInfo.mFrame.GetFA());
    arkParam.fp = reinterpret_cast<uintptr_t*>(fa);
    arkParam.sp = reinterpret_cast<uintptr_t*>(fa);
    
    const uint32_t* ip = uwContext.frameInfo.mFrame.GetIP();
    uintptr_t ipValue = reinterpret_cast<uintptr_t>(ip);
    arkParam.pc = &ipValue;
    
    bool isJsFrame = false;
    arkParam.isJsFrame = &isJsFrame;
    StepFrameType frameType = StepFrameType::JS_FRAME;
    arkParam.frameType = &frameType;
    arkParam.frameIndex = stack.size();
    
    int result = gStepArk(nullptr, nullptr, &arkParam);
    if (result != 1) {
        return false;
    }
    
    uwContext.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(arkParam.fp));
    uintptr_t pcValue = *arkParam.pc;
    uwContext.frameInfo.mFrame.SetIP(reinterpret_cast<const uint32_t*>(pcValue));
    
    return true;
}

StepArkFunc PrintStackInfoForMix::LoadArkTSStepArk()
{
    constexpr auto ARKTS_RUNTIME_LIB = "libark_jsruntime.so";
    constexpr auto STEP_ARK = "step_ark";
    
    void* handle = dlopen(ARKTS_RUNTIME_LIB, RTLD_LAZY);
    if (handle == nullptr) {
        LOG(RTLOG_ERROR, "Failed to load ArkTS runtime library: %s", dlerror());
        return nullptr;
    }
    
    StepArkFunc stepArk = reinterpret_cast<StepArkFunc>(dlsym(handle, STEP_ARK));
    if (stepArk == nullptr) {
        LOG(RTLOG_ERROR, "Failed to load step_ark function: %s", dlerror());
        dlclose(handle);
        return nullptr;
    }
    
    return stepArk;
}

bool PrintStackInfoForMix::ShouldStopUnwind(const MachineFrame& frame)
{
    if (frame.GetIP() == nullptr) {
        return true;
    }
    
    if (frame.GetFA() == nullptr) {
        return true;
    }
    
    uintptr_t faAddr = reinterpret_cast<uintptr_t>(frame.GetFA());
    if (faAddr < 0x1000 || faAddr > 0x7fffffffffff) {
        return true;
    }
    
    return false;
}

} // namespace MapleRuntime
