// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "os/Loader.h"

#include <dlfcn.h>

#include "Base/Log.h"

namespace MapleRuntime {
namespace Os {
namespace Loader {
void* LoadBinaryFile(const char* path)
{
    if (path == nullptr) {
        return nullptr;
    }
    void* handler = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (handler == nullptr) {
        LOG(RTLOG_ERROR, dlerror());
    }
    return handler;
}

int UnloadBinaryFile(void* handler)
{
    if (handler == nullptr) {
        return -1;
    }
    return dlclose(handler);
}

void* FindSymbol(void* handler, const char* symbolName)
{
    if (symbolName == nullptr) {
        return nullptr;
    }
    return dlsym(handler, symbolName);
}

int GetBinaryInfoFromAddress(const void* address, BinaryInfo* binInfo)
{
    if (address == nullptr) {
        return 0;
    }
    Dl_info dlInfo;
    int ret = dladdr(address, &dlInfo);
    if (!ret) {
        return 0;
    }
    binInfo->filePathName = const_cast<char*>(dlInfo.dli_fname);
    binInfo->symbolName = const_cast<char*>(dlInfo.dli_sname);
    return ret;
}
} // namespace Loader
} // namespace Os
} // namespace MapleRuntime
