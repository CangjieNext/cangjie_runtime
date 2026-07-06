// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MemMap.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#ifdef _WIN64
#include <errhandlingapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#endif

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/Panic.h"
#include "Base/SysCall.h"

namespace MapleRuntime {
using namespace std;

#if defined(__APPLE__)
namespace {

struct AppleMapResult {
    void* mappedAddr;
    bool taggedCangjieHeap;
};

AppleMapResult MapMemoryOnApple(void* reqBase, size_t reqSize, const MemMap::Option& opt)
{
    AppleMapResult result = { MAP_FAILED, false };
    result.taggedCangjieHeap = opt.tag != nullptr && std::strcmp(opt.tag, "cangjie_heap") == 0;
    unsigned int mmapFlags = static_cast<unsigned int>(opt.flags);
    if (result.taggedCangjieHeap) {
        mmapFlags &= ~static_cast<unsigned int>(MAP_NORESERVE);
    }
    int mmapFd = MrtAppleMmapFd(-1, opt.tag);
    // Tagged cangjie_heap: map RW directly (tag passed via fd). Others: PROT_NONE then mprotect.
    int mmapProt = result.taggedCangjieHeap ? opt.prot : PROT_NONE;
    result.mappedAddr =
        mmap(reqBase, reqSize, mmapProt, static_cast<int>(mmapFlags), mmapFd, 0);
    return result;
}

} // namespace
#endif

// not thread safe, do not call from multiple threads
MemMap* MemMap::MapMemory(size_t reqSize, size_t initSize, const Option& opt)
{
    void* mappedAddr = nullptr;
    bool taggedCangjieHeap = false;
    reqSize = AllocUtilRndUp<size_t>(reqSize, ALLOC_UTIL_PAGE_SIZE);

#ifdef _WIN64
    // Windows don`t support to map a non-access memory, and virtualProtect() only can change page protections on
    // memory blocks allocated by GlobalAlloc(), HeapAlloc(), or LocalAlloc(). For mapped views, this value must be
    // compatible with the access protection specified when the view was mapped, which means we can`t crete a non-access
    // map view, than change the protections like linux procedure. Here we just map a PAGE_EXECUTE_READWRITE at
    // beginning.
    mappedAddr = VirtualAlloc(NULL, reqSize, MEM_RESERVE, PAGE_READWRITE);
#else
    DLOG(ALLOC, "MemMap::MapMemory size %zu", reqSize);
#if defined(__APPLE__)
    AppleMapResult appleResult = MapMemoryOnApple(opt.reqBase, reqSize, opt);
    mappedAddr = appleResult.mappedAddr;
    taggedCangjieHeap = appleResult.taggedCangjieHeap;
#else
    mappedAddr = mmap(opt.reqBase, reqSize, PROT_NONE, opt.flags, -1, 0);
#endif
#endif

    bool failure = false;
#if defined(_WIN64)
    if (mappedAddr != NULL) {
#elif defined(__APPLE__)
    if (mappedAddr != MAP_FAILED) {
#else
    if (mappedAddr != MAP_FAILED) {
        (void)madvise(mappedAddr, reqSize, MADV_NOHUGEPAGE);
        MRT_PRCTL(mappedAddr, reqSize, opt.tag);
#endif
        size_t protSize = opt.protAll ? reqSize : initSize;
#if defined(__APPLE__)
        if (!taggedCangjieHeap && !ProtectMemInternal(mappedAddr, protSize, opt.prot)) {
#else
        if (!ProtectMemInternal(mappedAddr, protSize, opt.prot)) {
#endif
            failure = true;
            LOG(RTLOG_ERROR, "MemMap::MapMemory protect failed");
            ALLOCUTIL_MEM_UNMAP(mappedAddr, reqSize);
        }
    } else {
        failure = true;
#if !defined(_WIN64)
        LOG(RTLOG_ERROR, "MemMap::MapMemory mmap failed errno=%d tag=%s", errno,
            opt.tag != nullptr ? opt.tag : "(null)");
#endif
    }
    CHECK_DETAIL(!failure, "MemMap::MapMemory failed reqSize: %zu initSize: %zu", reqSize, initSize);

    DLOG(ALLOC, "MemMap::MapMemory size %zu successful at %p", reqSize, mappedAddr);
    MemMap* memMap = new (std::nothrow) MemMap(mappedAddr, initSize, reqSize);
    CHECK_DETAIL(memMap != nullptr, "new MemMap failed");
    return memMap;
}

#ifdef _WIN64
void MemMap::CommitMemory(void* addr, size_t size)
{
    CHECK_E(UNLIKELY(!VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE)),
        "VirtualAlloc commit failed in GetPage, errno: %d", GetLastError());
}
#endif

MemMap::MemMap(void* baseAddr, size_t initSize, size_t mappedSize)
    : memBaseAddr(baseAddr), memCurrSize(initSize), memMappedSize(mappedSize)
{
    memCurrEndAddr = reinterpret_cast<void*>(reinterpret_cast<MAddress>(memBaseAddr) + memCurrSize);
    memMappedEndAddr = reinterpret_cast<void*>(reinterpret_cast<MAddress>(memBaseAddr) + memMappedSize);
}

bool MemMap::ProtectMemInternal(void* addr, size_t size, int prot)
{
    DLOG(ALLOC, "MemMap::ProtectMem %p, size %zu, prot %d", addr, size, prot);
#ifdef _WIN64
    return true;
#else
    int ret = mprotect(addr, size, prot);
    return (ret == 0);
#endif
}
MemMap::~MemMap()
{
    ALLOCUTIL_MEM_UNMAP(memBaseAddr, memMappedSize);
    memBaseAddr = nullptr;
    memCurrEndAddr = nullptr;
    memMappedEndAddr = nullptr;
}
} // namespace MapleRuntime
