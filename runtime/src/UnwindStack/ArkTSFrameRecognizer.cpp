// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "ArkTSFrameRecognizer.h"

#include <dlfcn.h>
#include <link.h>

#include "Base/Log.h"

namespace MapleRuntime {

static bool gArkTSRuntimeAvailable = false;
static bool gArkTSRuntimeChecked = false;

bool ArkTSFrameRecognizer::IsArkTSRuntimeAvailable()
{
    if (gArkTSRuntimeChecked) {
        return gArkTSRuntimeAvailable;
    }
    
    gArkTSRuntimeChecked = true;
    
    constexpr auto ARKTS_RUNTIME_LIB = "libark_jsruntime.so";
    void* handle = dlopen(ARKTS_RUNTIME_LIB, RTLD_LAZY);
    if (handle == nullptr) {
        LOG(RTLOG_INFO, "ArkTS runtime library not available: %s", dlerror());
        gArkTSRuntimeAvailable = false;
        return false;
    }
    
    dlclose(handle);
    gArkTSRuntimeAvailable = true;
    return true;
}

bool ArkTSFrameRecognizer::IsArkTSFrame(const MachineFrame& frame)
{
    if (!IsArkTSRuntimeAvailable()) {
        return false;
    }
    
    return IsArkTSFrame(frame.GetFA());
}

bool ArkTSFrameRecognizer::IsArkTSFrame(FrameAddress* fa)
{
    if (fa == nullptr) {
        return false;
    }

    uintptr_t frameTypeAddr = reinterpret_cast<uintptr_t>(fa) - ARKTS_FRAME_TYPE_OFFSET;
    
    uintptr_t frameType = 0;
    if (frameTypeAddr < 0x1000 || frameTypeAddr > 0x7fffffffffff) {
        return false;
    }
    
    frameType = *reinterpret_cast<uintptr_t*>(frameTypeAddr);
    
    return IsValidArkTSFrameType(frameType);
}

bool ArkTSFrameRecognizer::IsValidArkTSFrameType(uintptr_t type)
{
    constexpr uintptr_t CLEAR_LAZY_DEOPT_FLAG = ~(1ULL << 30);
    uintptr_t actualType = type & CLEAR_LAZY_DEOPT_FLAG;
    
    return actualType >= 0 && actualType <= 30;
}

bool ArkTSFrameRecognizer::ExtractFrameInfo(FrameAddress* fa, ArkTSFrameInfo& info)
{
    if (!IsArkTSFrame(fa)) {
        return false;
    }
    
    info.framePtr = fa;
    info.returnAddr = GetReturnAddr(fa);
    
    Dl_info dlInfo;
    if (dladdr(reinterpret_cast<const void*>(info.returnAddr), &dlInfo)) {
        info.fileName = CString(dlInfo.dli_fname);
    }
    
    return true;
}

FrameAddress* ArkTSFrameRecognizer::GetPrevFp(FrameAddress* fa)
{
    if (fa == nullptr) {
        return nullptr;
    }
    
    uintptr_t prevFpOffset = ARKTS_FRAME_TYPE_OFFSET + sizeof(uintptr_t);
    uintptr_t prevFpAddr = reinterpret_cast<uintptr_t>(fa) - prevFpOffset;
    
    if (prevFpAddr < 0x1000 || prevFpAddr > 0x7fffffffffff) {
        return nullptr;
    }
    
    return *reinterpret_cast<FrameAddress**>(prevFpAddr);
}

const uint32_t* ArkTSFrameRecognizer::GetReturnAddr(FrameAddress* fa)
{
    if (fa == nullptr) {
        return nullptr;
    }
    
    uintptr_t returnAddrOffset = ARKTS_FRAME_TYPE_OFFSET + sizeof(uintptr_t) * 2;
    uintptr_t returnAddrAddr = reinterpret_cast<uintptr_t>(fa) - returnAddrOffset;
    
    if (returnAddrAddr < 0x1000 || returnAddrAddr > 0x7fffffffffff) {
        return nullptr;
    }
    
    return *reinterpret_cast<const uint32_t**>(returnAddrAddr);
}

} // namespace MapleRuntime
