// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "ArkTSFrameRecognizer.h"

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>

#include "Base/Log.h"

namespace MapleRuntime {

static bool gArkTSRuntimeAvailable = false;
static bool gArkTSRuntimeChecked = false;

uintptr_t ArkTSFrameRecognizer::arkStubBegin_ = 0;
uintptr_t ArkTSFrameRecognizer::arkStubEnd_ = 0;
uintptr_t ArkTSFrameRecognizer::arkInterpreterBegin_ = 0;
uintptr_t ArkTSFrameRecognizer::arkInterpreterEnd_ = 0;
bool ArkTSFrameRecognizer::arkRangeInitialized_ = false;

static pthread_mutex_t gArkRangeMutex = PTHREAD_MUTEX_INITIALIZER;

bool ArkTSFrameRecognizer::IsArkTSRuntimeAvailable()
{
    if (gArkTSRuntimeChecked) {
        return gArkTSRuntimeAvailable;
    }
    
    gArkTSRuntimeChecked = true;
    
    constexpr auto ARK_RUNTIME_LIB = "libark_jsruntime.so";
    void* handle = dlopen(ARK_RUNTIME_LIB, RTLD_LAZY);
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
    
    uintptr_t pc = reinterpret_cast<uintptr_t>(frame.GetIP());
    return IsArkTSFrameByPc(pc);
}

bool ArkTSFrameRecognizer::IsArkTSFrame(FrameAddress* fa)
{
    if (fa == nullptr || !IsArkTSRuntimeAvailable()) {
        return false;
    }
    
    uintptr_t pc = reinterpret_cast<uintptr_t>(*reinterpret_cast<uintptr_t*>(fa));
    return IsArkTSFrameByPc(pc);
}

static uintptr_t StripPac(uintptr_t pc)
{
#ifdef __aarch64__
    constexpr uintptr_t PAC_MASK = 0x0000007fffffffffULL;
    return pc & PAC_MASK;
#else
    return pc;
#endif
}

bool ArkTSFrameRecognizer::IsArkTSFrameByPc(uintptr_t pc)
{
    return IsInInterpreterRange(pc);
}

bool ArkTSFrameRecognizer::IsInStubRange(uintptr_t pc)
{
    if (pc == 0) {
        return false;
    }
    
    uintptr_t strippedPc = StripPac(pc);
    
    pthread_mutex_lock(&gArkRangeMutex);
    if (!arkRangeInitialized_) {
        InitArkRanges();
        arkRangeInitialized_ = true;
    }
    pthread_mutex_unlock(&gArkRangeMutex);
    
    bool inStubRange = (arkStubBegin_ != 0 && arkStubEnd_ != 0) &&
                       (strippedPc >= arkStubBegin_ && strippedPc < arkStubEnd_);
    
    LOG(RTLOG_INFO, "[IsInStubRange] pc=0x%lx, strippedPc=0x%lx, stubRange=[0x%lx,0x%lx], inStub=%d",
        pc, strippedPc, arkStubBegin_, arkStubEnd_, inStubRange);
    
    return inStubRange;
}

bool ArkTSFrameRecognizer::IsInInterpreterRange(uintptr_t pc)
{
    if (pc == 0) {
        return false;
    }
    
    uintptr_t strippedPc = StripPac(pc);
    
    pthread_mutex_lock(&gArkRangeMutex);
    if (!arkRangeInitialized_) {
        InitArkRanges();
        arkRangeInitialized_ = true;
    }
    pthread_mutex_unlock(&gArkRangeMutex);
    
    bool inInterpreterRange = (arkInterpreterBegin_ != 0 && arkInterpreterEnd_ != 0) &&
                              (strippedPc >= arkInterpreterBegin_ && strippedPc < arkInterpreterEnd_);
    
    LOG(RTLOG_INFO, "[IsInInterpreterRange] pc=0x%lx, strippedPc=0x%lx, interpRange=[0x%lx,0x%lx], inInterp=%d",
        pc, strippedPc, arkInterpreterBegin_, arkInterpreterEnd_, inInterpreterRange);
    
    return inInterpreterRange;
}

bool ArkTSFrameRecognizer::InitArkRanges()
{
    InitArkStubRange();
    InitArkInterpreterRange();
    
    LOG(RTLOG_INFO, "[InitArkRanges] stub=[0x%lx,0x%lx], interpreter=[0x%lx,0x%lx]",
        arkStubBegin_, arkStubEnd_, arkInterpreterBegin_, arkInterpreterEnd_);
    
    return (arkStubBegin_ != 0 || arkInterpreterBegin_ != 0);
}

bool ArkTSFrameRecognizer::InitArkStubRange()
{
    FILE* mapsFile = fopen("/proc/self/maps", "r");
    if (mapsFile == nullptr) {
        LOG(RTLOG_INFO, "[InitArkStubRange] Failed to open /proc/self/maps");
        return false;
    }
    
    char line[512];
    uintptr_t begin = 0;
    uintptr_t end = 0;
    
    while (fgets(line, sizeof(line), mapsFile) != nullptr) {
        uintptr_t mapBegin;
        uintptr_t mapEnd;
        char perms[5];
        unsigned long offset;
        unsigned long major;
        unsigned long minor;
        unsigned long inode;
        char pathname[256] = {0};
        
        int parsed = sscanf(line, "%lx-%lx %4s %lx %lx:%lx %lu %255s",
                            &mapBegin, &mapEnd, perms, &offset, &major, &minor, &inode, pathname);
        
        if (parsed < 7) {
            continue;
        }
        
        bool isExec = (perms[2] == 'x');
        if (!isExec) {
            continue;
        }
        
        bool isArkStub = (strstr(pathname, "stub.an") != nullptr);
        bool isArkAnon = (strncmp(pathname, "[anon:ArkTS Code", 16) == 0);
        bool isDevZero = (strcmp(pathname, "/dev/zero") == 0);
        
        if (isArkStub || isArkAnon || isDevZero) {
            if (begin == 0 || mapBegin < begin) {
                begin = mapBegin;
            }
            if (mapEnd > end) {
                end = mapEnd;
            }
            LOG(RTLOG_INFO, "[InitArkStubRange] Found ark map: %s, range=[0x%lx,0x%lx]",
                pathname, mapBegin, mapEnd);
        }
    }
    
    fclose(mapsFile);
    
    arkStubBegin_ = begin;
    arkStubEnd_ = end;
    
    LOG(RTLOG_INFO, "[InitArkStubRange] Final stub range: [0x%lx,0x%lx]", begin, end);
    
    return (begin != 0 && end != 0);
}

bool ArkTSFrameRecognizer::InitArkInterpreterRange()
{
    constexpr auto ARK_LIB_NAME = "libarkruntime.so";
    
    void* handle = dlopen(ARK_LIB_NAME, RTLD_LAZY);
    if (handle == nullptr) {
        LOG(RTLOG_INFO, "[InitArkInterpreterRange] Cannot open %s: %s", ARK_LIB_NAME, dlerror());
        return false;
    }
    
    void* nopHandle = dlsym(handle, "HANDLE_FAST_NOP");
    void* excepHandle = dlsym(handle, "HANDLE_FAST_EXCEPTION");
    
    if (nopHandle == nullptr || excepHandle == nullptr) {
        LOG(RTLOG_INFO, "[InitArkInterpreterRange] Cannot find HANDLE_FAST_NOP or HANDLE_FAST_EXCEPTION");
        dlclose(handle);
        return false;
    }
    
    arkInterpreterBegin_ = reinterpret_cast<uintptr_t>(nopHandle);
    arkInterpreterEnd_ = reinterpret_cast<uintptr_t>(excepHandle) + 64;
    
    LOG(RTLOG_INFO, "[InitArkInterpreterRange] interpreter range: 0x%lx - 0x%lx",
        arkInterpreterBegin_, arkInterpreterEnd_);
    
    dlclose(handle);
    return true;
}

bool ArkTSFrameRecognizer::ExtractFrameInfo(FrameAddress* fa, ArkTSFrameInfo& info)
{
    if (fa == nullptr) {
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
    
    uintptr_t prevFpAddr = reinterpret_cast<uintptr_t>(fa);
    
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
    
    uintptr_t returnAddrAddr = reinterpret_cast<uintptr_t>(fa) + sizeof(uintptr_t);
    
    if (returnAddrAddr < 0x1000 || returnAddrAddr > 0x7fffffffffff) {
        return nullptr;
    }
    
    return *reinterpret_cast<const uint32_t**>(returnAddrAddr);
}

} // namespace MapleRuntime