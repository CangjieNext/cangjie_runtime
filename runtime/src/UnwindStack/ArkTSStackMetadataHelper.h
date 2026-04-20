// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ARKTS_STACK_METADATA_HELPER_H
#define MRT_ARKTS_STACK_METADATA_HELPER_H

#include <cstdint>
#include <dlfcn.h>

#include "Base/Types.h"
#include "Base/Log.h"

namespace MapleRuntime {

constexpr int ARKTS_FUNCTIONNAME_MAX = 1024;
constexpr int ARKTS_PACKAGENAME_MAX = 1024;
constexpr int ARKTS_URL_MAX = 1024;

struct ArkTSJsFrame {
    char functionName[ARKTS_FUNCTIONNAME_MAX];
    char packageName[ARKTS_PACKAGENAME_MAX];
    char url[ARKTS_URL_MAX];
    int32_t line;
    int32_t column;
};

bool InitArkTSUnwinder();
void CleanupArkTSUnwinder();

class ArkTSStackMetadataHelper {
public:
    explicit ArkTSStackMetadataHelper(uint64_t ip, uint64_t pc)
        : ip_(ip), pc_(pc), lineNumber_(0), methodName_("ArkTS_Function"),
          className_(""), fileName_("ArkTS"), isValid_(false)
    {
        ExtractInfoFromIP();
    }

    ~ArkTSStackMetadataHelper() = default;

    uint32_t GetLineNumber() const { return lineNumber_; }
    CString GetMethodName() const { return methodName_; }
    CString GetClassName() const { return className_; }
    CString GetFileName() const { return fileName_; }

    bool IsValid() const { return isValid_; }

private:
    void ExtractInfoFromIP();
    static uintptr_t GetMapBaseForAddress(uintptr_t address);
    static uintptr_t GetLoadOffsetForAddress(uintptr_t address);

    uint64_t ip_;
    uint64_t pc_;
    uint32_t lineNumber_;
    CString methodName_;
    CString className_;
    CString fileName_;
    bool isValid_;
};

} // namespace MapleRuntime

#endif // MRT_ARKTS_STACK_METADATA_HELPER_H