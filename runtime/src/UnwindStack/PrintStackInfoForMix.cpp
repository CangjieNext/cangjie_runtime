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
#include "Common/StackType.h"

namespace MapleRuntime {

bool IsFrameValid(const MachineFrame& mFrame)
{
    if (mFrame.GetIP() == nullptr || mFrame.GetFA() == nullptr) {
        return false;
    }
    uintptr_t faAddr = reinterpret_cast<uintptr_t>(mFrame.GetFA());
    if (faAddr < 0x1000 || faAddr > 0x7fffffffffff) {
        return false;
    }
    return true;
}

void PrintStackInfoForMix::FillInStackTrace()
{
    UnwindContext uwContext;
    CheckTopUnwindContextAndInit(uwContext);
    
    anchorFA = nullptr;
    
    uintptr_t prevPC = 0;
    uintptr_t prevFA = 0;
    size_t repeatCount = 0;
    constexpr size_t MAX_REPEAT = 3;
    
    LOG(RTLOG_INFO, "[FillInStackTrace] Start filling stack trace for mixed stack");
    
    while (!ShouldStopUnwind(uwContext.frameInfo.mFrame)) {
        uintptr_t currentPC = reinterpret_cast<uintptr_t>(uwContext.frameInfo.mFrame.GetIP());
        uintptr_t currentFA = reinterpret_cast<uintptr_t>(uwContext.frameInfo.mFrame.GetFA());
        
        if (currentPC == prevPC && currentFA == prevFA) {
            repeatCount++;
            if (repeatCount >= MAX_REPEAT) {
                LOG(RTLOG_INFO, "[FillInStackTrace] Detected repeated frame, stopping unwind");
                break;
            }
        } else {
            repeatCount = 0;
        }
        
        prevPC = currentPC;
        prevFA = currentFA;
        
        AnalyseAndSetFrameType(uwContext);
        FrameType currentType = uwContext.frameInfo.GetFrameType();
        
        if (currentType == FrameType::MANAGED) {
            const uint32_t* ip = uwContext.frameInfo.mFrame.GetIP();
            const uint32_t* startPC = uwContext.frameInfo.GetFuncStartPC();
            LOG(RTLOG_INFO, "[FillInStackTrace] MANAGED frame: ip=0x%lx, startPC=0x%lx",
                reinterpret_cast<uintptr_t>(ip), reinterpret_cast<uintptr_t>(startPC));
            stack.emplace_back(uwContext.frameInfo);
        } else if (currentType == FrameType::ARKTS_FRAME) {
            const uint32_t* ip = uwContext.frameInfo.mFrame.GetIP();
            FrameAddress* fa = uwContext.frameInfo.mFrame.GetFA();
            LOG(RTLOG_INFO, "[FillInStackTrace] ARKTS_FRAME: ip=0x%lx, fa=0x%lx",
                reinterpret_cast<uintptr_t>(ip), reinterpret_cast<uintptr_t>(fa));
            stack.emplace_back(uwContext.frameInfo);
        } else if (currentType == FrameType::ARKTS_STUB) {
            const uint32_t* ip = uwContext.frameInfo.mFrame.GetIP();
            LOG(RTLOG_INFO, "[FillInStackTrace] ARKTS_STUB frame: ip=0x%lx",
                reinterpret_cast<uintptr_t>(ip));
        } else if (currentType == FrameType::NATIVE) {
            const uint32_t* ip = uwContext.frameInfo.mFrame.GetIP();
            LOG(RTLOG_INFO, "[FillInStackTrace] NATIVE frame: ip=0x%lx (skipped)",
                reinterpret_cast<uintptr_t>(ip));
        } else if (currentType == FrameType::N2C_STUB) {
            LOG(RTLOG_INFO, "[FillInStackTrace] N2C_STUB frame");
        } else if (currentType == FrameType::C2N_STUB) {
            LOG(RTLOG_INFO, "[FillInStackTrace] C2N_STUB frame");
        } else if (currentType == FrameType::RUNTIME) {
            LOG(RTLOG_INFO, "[FillInStackTrace] RUNTIME frame");
        }
        
        lastFrameType = currentType;
        
        if (currentType == FrameType::ARKTS_FRAME || currentType == FrameType::ARKTS_STUB) {
            bool isJsFrame = false;
            if (!UnwindToCallerArkTSFrame(uwContext, isJsFrame)) {
                break;
            }
            if (isJsFrame) {
                uwContext.frameInfo.SetFrameType(FrameType::ARKTS_FRAME);
            } else {
                uwContext.frameInfo.SetFrameType(FrameType::UNKNOWN);
            }
        } else {
            UnwindContext caller;
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

bool PrintStackInfoForMix::AccessMem(void* ctx, uintptr_t addr, uintptr_t* val)
{
    (void)ctx;
    if (val == nullptr) {
        return false;
    }
    
    if (addr < 0x1000 || addr > 0x7fffffffffff) {
        return false;
    }
    
    *val = *reinterpret_cast<uintptr_t*>(addr);
    return true;
}

bool PrintStackInfoForMix::UnwindToCallerArkTSFrame(UnwindContext& uwContext, bool& outIsJsFrame)
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
        outIsJsFrame = false;
        return true;
    }
    
    ArkStepParam arkParam;
    FrameAddress* fa = const_cast<FrameAddress*>(uwContext.frameInfo.mFrame.GetFA());
    uintptr_t fpValue = reinterpret_cast<uintptr_t>(fa);
    uintptr_t spValue = fpValue;
    uintptr_t pcValue = reinterpret_cast<uintptr_t>(uwContext.frameInfo.mFrame.GetIP());
    
    arkParam.fp = &fpValue;
    arkParam.sp = &spValue;
    arkParam.pc = &pcValue;
    
    bool isJsFrame = false;
    arkParam.isJsFrame = &isJsFrame;
    StepFrameType frameType = StepFrameType::JS_FRAME;
    arkParam.frameType = &frameType;
    arkParam.frameIndex = stack.size();
    
    int result = gStepArk(nullptr, AccessMem, &arkParam);
    if (result != 1) {
        LOG(RTLOG_ERROR, "[UnwindToCallerArkTSFrame] step_ark failed with result=%d, pc=0x%lx, fp=0x%lx", 
            result, pcValue, fpValue);
        return false;
    }
    
    uwContext.frameInfo.mFrame.SetFA(reinterpret_cast<FrameAddress*>(fpValue));
    uwContext.frameInfo.mFrame.SetIP(reinterpret_cast<const uint32_t*>(pcValue));
    outIsJsFrame = isJsFrame;
    
    LOG(RTLOG_INFO, "[UnwindToCallerArkTSFrame] step_ark success: new_pc=0x%lx, new_fp=0x%lx, new_sp=0x%lx, isJsFrame=%d",
        pcValue, fpValue, spValue, isJsFrame);
    
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
