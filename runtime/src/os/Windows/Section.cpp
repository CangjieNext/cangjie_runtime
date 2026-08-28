// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <windows.h>
#include <limits>

namespace {
struct MetadataSection {
    uintptr_t address;
    unsigned long size;
};
}

extern "C" {
// CJMetadataHeader
unsigned long __CJMetadataSize;
uintptr_t __CJMetadataStart;

// CJSDKVersion
unsigned long __CJSDKVersionSize;
uintptr_t __CJSDKVersion;

// CJMethodInfo
unsigned long __CJMethodInfoSize;
uintptr_t __CJMethodInfo;

// CJGlobalInitFunc
unsigned long __CJGlobalInitFuncSize;
uintptr_t __CJGlobalInitFunc;

// CJStringPoolDict
unsigned long __CJStringPoolDictSize;
uintptr_t __CJStringPoolDict;

// CJStringPool
unsigned long __CJStringPoolSize;
uintptr_t __CJStringPool;

// CJStackMap
unsigned long __CJStackMapSize;
uintptr_t __CJStackMap;

// CJGCTib
unsigned long __CJGCTibSize;
uintptr_t __CJGCTib;

// CJGCRoots
unsigned long __CJGCRootsSize;
uintptr_t __CJGCRoots;

// CJTypeTemplate
unsigned long __CJTypeTemplateSize;
uintptr_t __CJTypeTemplate;

// CJTypeInfo
unsigned long __CJTypeInfoSize;
uintptr_t __CJTypeInfo;

// CJTypeInfoFields
unsigned long __CJTypeFieldsSize;
uintptr_t __CJTypeFields;

// CJMTable
unsigned long __CJMTableSize;
uintptr_t __CJMTable;

unsigned long __CJInnerTypeExtensionsSize;
uintptr_t __CJInnerTypeExtensions;

unsigned long __CJOuterTypeExtensionsSize;
uintptr_t __CJOuterTypeExtensions;

// Static GI
unsigned long __CJStaticGITableSize;
uintptr_t __CJStaticGITable;

// CJGCFlags
unsigned long __CJGCFlagsSize;
uintptr_t __CJGCFlags;

// CJReflectPkgInfo
unsigned long __CJGCReflectPkgInfoSize;
uintptr_t __CJReflectPkgInfo;

// CJReflectGV
unsigned long __CJReflectGVSize;
uintptr_t __CJReflectGV;

// CJReflectGI
unsigned long __CJReflectGISize;
uintptr_t __CJReflectGI;

// CJTypeExt
unsigned long __CJTypeExtSize;
uintptr_t __CJTypeExt;

uintptr_t g_runtimeStaticStart;
uintptr_t g_runtimeStaticEnd;

__attribute__((constructor(0))) __declspec(dllexport) void InitData()
{
    HMODULE hModule = NULL;
    if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(&InitData),
                           &hModule)) {
        return;
    }
    if (hModule == NULL) {
        return;
    }

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hModule;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }

    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((BYTE*)hModule + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }

    PIMAGE_SECTION_HEADER sectionHeader = IMAGE_FIRST_SECTION(ntHeaders);

    for (UINT i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
        const char* secName = reinterpret_cast<const char*>(sectionHeader->Name);

        if (strncmp(secName, ".header", sizeof(".header") - 1) == 0) {
            __CJMetadataStart = reinterpret_cast<uintptr_t>(hModule) +
                                sectionHeader->VirtualAddress;
            __CJMetadataSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjsdkv", sizeof(".cjsdkv") - 1) == 0) {
            __CJSDKVersion = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJSDKVersionSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjmthd", sizeof(".cjmthd") - 1) == 0) {
            __CJMethodInfo = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJMethodInfoSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjinitF", sizeof(".cjinitF") - 1) == 0) {
            __CJGlobalInitFunc = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJGlobalInitFuncSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjspdct", sizeof(".cjspdct") - 1) == 0) {
            __CJStringPoolDict = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJStringPoolDictSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjsp", sizeof(".cjsp") - 1) == 0) {
            __CJStringPool = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJStringPoolSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjsm", sizeof(".cjsm") - 1) == 0) {
            __CJStackMap = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJStackMapSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjgctib", sizeof(".cjgctib") - 1) == 0) {
            __CJGCTib = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJGCTibSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjgcrts", sizeof(".cjgcrts") - 1) == 0) {
            __CJGCRoots = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJGCRootsSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjtt", sizeof(".cjtt") - 1) == 0) {
            __CJTypeTemplate = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJTypeTemplateSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjti", sizeof(".cjti") - 1) == 0) {
            __CJTypeInfo = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJTypeInfoSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjfield", sizeof(".cjfield") - 1) == 0) {
            __CJTypeFields = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJTypeFieldsSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjmtbl", sizeof(".cjmtbl") - 1) == 0) {
            __CJMTable = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJMTableSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjinty", sizeof(".cjinty") - 1) == 0) {
            __CJInnerTypeExtensions = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJInnerTypeExtensionsSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjouty", sizeof(".cjouty") - 1) == 0) {
            __CJOuterTypeExtensions = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJOuterTypeExtensionsSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjsgt", sizeof(".cjsgt") - 1) == 0) {
            __CJStaticGITable = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJStaticGITableSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjgcflg", sizeof(".cjgcflg") - 1) == 0) {
            __CJGCFlags = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJGCFlagsSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjrflp", sizeof(".cjrflp") - 1) == 0) {
            __CJReflectPkgInfo = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJGCReflectPkgInfoSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjrflv", sizeof(".cjrflv") - 1) == 0) {
            __CJReflectGV = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJReflectGVSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjrflg", sizeof(".cjrflg") - 1) == 0) {
            __CJReflectGI = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJReflectGISize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".cjtpe", sizeof(".cjtpe") - 1) == 0) {
            __CJTypeExt = reinterpret_cast<uintptr_t>(hModule) +
                             sectionHeader->VirtualAddress;
            __CJTypeExtSize = sectionHeader->Misc.VirtualSize;
        } else if (strncmp(secName, ".text_rt", sizeof(".text_rt") - 1) == 0) {
            g_runtimeStaticStart = reinterpret_cast<uintptr_t>(hModule) +
                                   sectionHeader->VirtualAddress;
            g_runtimeStaticEnd =
                g_runtimeStaticStart + sectionHeader->Misc.VirtualSize;
        }
        ++sectionHeader;
    }

    const MetadataSection metadataSections[] = {
        { __CJSDKVersion, __CJSDKVersionSize },
        { __CJMethodInfo, __CJMethodInfoSize },
        { __CJGlobalInitFunc, __CJGlobalInitFuncSize },
        { __CJStringPoolDict, __CJStringPoolDictSize },
        { __CJStringPool, __CJStringPoolSize },
        { __CJStackMap, __CJStackMapSize },
        { __CJGCTib, __CJGCTibSize },
        { __CJGCRoots, __CJGCRootsSize },
        { __CJTypeTemplate, __CJTypeTemplateSize },
        { __CJTypeInfo, __CJTypeInfoSize },
        { __CJTypeFields, __CJTypeFieldsSize },
        { __CJMTable, __CJMTableSize },
        { __CJInnerTypeExtensions, __CJInnerTypeExtensionsSize },
        { __CJOuterTypeExtensions, __CJOuterTypeExtensionsSize },
        { __CJStaticGITable, __CJStaticGITableSize },
        { __CJGCFlags, __CJGCFlagsSize },
        { __CJReflectPkgInfo, __CJGCReflectPkgInfoSize },
        { __CJReflectGV, __CJReflectGVSize },
        { __CJReflectGI, __CJReflectGISize },
        { __CJTypeExt, __CJTypeExtSize },
    };

    uintptr_t metadataStart = 0;
    uintptr_t metadataEnd = 0;
    for (const MetadataSection& section : metadataSections) {
        // Empty Cangjie metadata categories do not have a PE section. Their
        // address and size remain zero and must not participate in min/max.
        if (section.address == 0 || section.size == 0 ||
            section.address > std::numeric_limits<uintptr_t>::max() - section.size) {
            continue;
        }
        const uintptr_t sectionEnd = section.address + section.size;
        if (metadataStart == 0 || section.address < metadataStart) {
            metadataStart = section.address;
        }
        if (sectionEnd > metadataEnd) {
            metadataEnd = sectionEnd;
        }
    }

    if (metadataStart == 0 || metadataEnd < metadataStart) {
        __CJMetadataSize = 0;
        return;
    }
    const uintptr_t metadataSize = metadataEnd - metadataStart;
    __CJMetadataSize = metadataSize <= std::numeric_limits<unsigned long>::max()
                           ? static_cast<unsigned long>(metadataSize)
                           : 0;
}
}
