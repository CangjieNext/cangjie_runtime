// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "ArkTSStackMetadataHelper.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <string>
#include <map>

#include "Base/Log.h"

#ifdef __OHOS__
extern "C" unsigned long long CJGetArkVM();
#endif

namespace MapleRuntime {

using ArkTsCreateJsSymbolExtractorFunc = int (*)(uintptr_t* extractorptr);
using ArkTsDestroyJsSymbolExtractorFunc = int (*)(uintptr_t extractorptr);
using ArkTsParseJsFileInfoFunc = int (*)(uintptr_t byteCodePc, uintptr_t mapBase, uintptr_t offset,
                                         const char* filePath, uintptr_t extractorptr,
                                         ArkTSJsFrame* jsFunction);
using ArkTsParseJsFrameInfoFunc = int (*)(uintptr_t byteCodePc, uintptr_t mapBase, uintptr_t loadOffset,
                                         uint8_t* data, uint64_t dataSize, uintptr_t extractorptr,
                                         ArkTSJsFrame* jsFunction);
using ArkTsParseJsFrameInfoLocalFunc = int (*)(uintptr_t byteCodePc, uintptr_t mapBase, uintptr_t loadOffset,
                                                ArkTSJsFrame* jsFunction);
using ArkTsCreateLocalFunc = int (*)();
using ArkTsDestroyLocalFunc = int (*)();

#ifdef __OHOS__
using ArkTsTranslateJsStackInfoFunc = void (*)(uintptr_t vm, std::string& url, int& line, int& column, std::string& packageName);
#endif

static constexpr auto ARKTS_RUNTIME_LIB = "libark_jsruntime.so";
static constexpr auto ARKTS_CREATE_JS_SYMBOL_EXTRACTOR = "ark_create_js_symbol_extractor";
static constexpr auto ARKTS_DESTROY_JS_SYMBOL_EXTRACTOR = "ark_destory_js_symbol_extractor";
static constexpr auto ARKTS_PARSE_JS_FILE_INFO = "ark_parse_js_file_info";
static constexpr auto ARKTS_PARSE_JS_FRAME_INFO = "ark_parse_js_frame_info";
static constexpr auto ARKTS_PARSE_JS_FRAME_INFO_LOCAL = "ark_parse_js_frame_info_local";
static constexpr auto ARKTS_CREATE_LOCAL = "ark_create_local";
static constexpr auto ARKTS_DESTROY_LOCAL = "ark_destroy_local";

#ifdef __OHOS__
static constexpr auto ARKTS_TRANSLATE_JS_STACK_INFO =
    "_ZN5panda9DFXJSNApi20TranslateJSStackInfoEPKNS_10ecmascript6EcmaVMERNSt3__h12basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEERiSD_SC_";
#endif

static void* gArkTsLibHandle = nullptr;
static ArkTsCreateJsSymbolExtractorFunc gArkTsCreateExtractor = nullptr;
static ArkTsDestroyJsSymbolExtractorFunc gArkTsDestroyExtractor = nullptr;
static ArkTsParseJsFileInfoFunc gArkTsParseFileInfo = nullptr;
static ArkTsParseJsFrameInfoFunc gArkTsParseFrameInfo = nullptr;
static ArkTsParseJsFrameInfoLocalFunc gArkTsParseFrameInfoLocal = nullptr;
static ArkTsCreateLocalFunc gArkTsCreateLocal = nullptr;
static ArkTsDestroyLocalFunc gArkTsDestroyLocal = nullptr;

#ifdef __OHOS__
static ArkTsTranslateJsStackInfoFunc gArkTsTranslateJsStackInfo = nullptr;
#endif

static pthread_mutex_t gArkTsMutex = PTHREAD_MUTEX_INITIALIZER;
static bool gArkTsInitialized = false;
static bool gArkTsInitAttempted = false;

static std::map<std::string, uintptr_t> gExtractorCache;
static std::map<uintptr_t, std::string> gExtractorToPath;

struct MapInfo {
    uintptr_t begin;
    uintptr_t end;
    uintptr_t offset;
    char pathname[256];
};

static bool GetMapInfoForAddress(uintptr_t address, MapInfo& info)
{
    FILE* mapsFile = fopen("/proc/self/maps", "r");
    if (mapsFile == nullptr) {
        LOG(RTLOG_INFO, "[GetMapInfoForAddress] Failed to open /proc/self/maps");
        return false;
    }

    char line[512];
    while (fgets(line, sizeof(line), mapsFile) != nullptr) {
        uintptr_t begin = 0;
        uintptr_t end = 0;
        char perms[5] = {0};
        uintptr_t offset = 0;
        uintptr_t major = 0;
        uintptr_t minor = 0;
        unsigned long inode = 0;
        char pathname[256] = {0};

        int parsed = sscanf(line, "%lx-%lx %4s %lx %lx:%lx %lu %255s",
                            &begin, &end, perms, &offset, &major, &minor, &inode, pathname);

        if (parsed < 7) {
            continue;
        }

        if (address >= begin && address < end) {
            fclose(mapsFile);
            info.begin = begin;
            info.end = end;
            info.offset = offset;
            strncpy(info.pathname, pathname, sizeof(info.pathname) - 1);
            info.pathname[sizeof(info.pathname) - 1] = '\0';
            LOG(RTLOG_INFO, "[GetMapInfoForAddress] Found map: addr=0x%lx, begin=0x%lx, end=0x%lx, offset=0x%lx, path=%s",
                address, begin, end, offset, pathname);
            return true;
        }
    }

    fclose(mapsFile);
    LOG(RTLOG_INFO, "[GetMapInfoForAddress] No map found for address 0x%lx", address);
    return false;
}

uintptr_t ArkTSStackMetadataHelper::GetMapBaseForAddress(uintptr_t address)
{
    MapInfo info;
    if (GetMapInfoForAddress(address, info)) {
        return info.begin;
    }
    return 0;
}

uintptr_t ArkTSStackMetadataHelper::GetLoadOffsetForAddress(uintptr_t address)
{
    MapInfo info;
    if (GetMapInfoForAddress(address, info)) {
        return info.offset;
    }
    return 0;
}

static bool IsHapMap(const char* pathname)
{
    if (pathname == nullptr || pathname[0] == '\0') {
        return false;
    }
    const char* hapSuffix = ".hap";
    size_t hapLen = strlen(hapSuffix);
    size_t pathLen = strlen(pathname);
    if (pathLen > hapLen && strcmp(pathname + pathLen - hapLen, hapSuffix) == 0) {
        return true;
    }
    return false;
}

static bool IsArkCodeMap(const char* pathname)
{
    if (pathname == nullptr || pathname[0] == '\0') {
        return false;
    }
    const char* arkCodeSuffix = ".ark";
    size_t arkLen = strlen(arkCodeSuffix);
    size_t pathLen = strlen(pathname);
    if (pathLen > arkLen && strcmp(pathname + pathLen - arkLen, arkCodeSuffix) == 0) {
        return true;
    }
    return false;
}

static uintptr_t GetOrCreateExtractorForPath(const char* filePath)
{
    if (filePath == nullptr || filePath[0] == '\0') {
        return 0;
    }
    
    std::string pathStr(filePath);
    
    pthread_mutex_lock(&gArkTsMutex);
    
    auto it = gExtractorCache.find(pathStr);
    if (it != gExtractorCache.end()) {
        pthread_mutex_unlock(&gArkTsMutex);
        return it->second;
    }
    
    uintptr_t extractorPtr = 0;
    if (gArkTsCreateExtractor(&extractorPtr) < 0) {
        LOG(RTLOG_INFO, "[GetOrCreateExtractorForPath] Failed to create extractor for path: %s", filePath);
        pthread_mutex_unlock(&gArkTsMutex);
        return 0;
    }
    
    gExtractorCache[pathStr] = extractorPtr;
    gExtractorToPath[extractorPtr] = pathStr;
    
    LOG(RTLOG_INFO, "[GetOrCreateExtractorForPath] Created new extractor=%lu for path: %s", extractorPtr, filePath);
    
    pthread_mutex_unlock(&gArkTsMutex);
    return extractorPtr;
}

bool InitArkTSUnwinder()
{
    pthread_mutex_lock(&gArkTsMutex);
    
    if (gArkTsInitialized) {
        pthread_mutex_unlock(&gArkTsMutex);
        return true;
    }
    
    if (gArkTsInitAttempted) {
        pthread_mutex_unlock(&gArkTsMutex);
        return false;
    }
    
    gArkTsInitAttempted = true;
    
    gArkTsLibHandle = dlopen(ARKTS_RUNTIME_LIB, RTLD_LAZY);
    if (gArkTsLibHandle == nullptr) {
        LOG(RTLOG_INFO, "[InitArkTSUnwinder] Failed to load ArkTS runtime library: %s", dlerror());
        pthread_mutex_unlock(&gArkTsMutex);
        return false;
    }
    
    gArkTsCreateExtractor = reinterpret_cast<ArkTsCreateJsSymbolExtractorFunc>(
        dlsym(gArkTsLibHandle, ARKTS_CREATE_JS_SYMBOL_EXTRACTOR));
    
    gArkTsDestroyExtractor = reinterpret_cast<ArkTsDestroyJsSymbolExtractorFunc>(
        dlsym(gArkTsLibHandle, ARKTS_DESTROY_JS_SYMBOL_EXTRACTOR));
    
    gArkTsParseFileInfo = reinterpret_cast<ArkTsParseJsFileInfoFunc>(
        dlsym(gArkTsLibHandle, ARKTS_PARSE_JS_FILE_INFO));
    
    gArkTsParseFrameInfo = reinterpret_cast<ArkTsParseJsFrameInfoFunc>(
        dlsym(gArkTsLibHandle, ARKTS_PARSE_JS_FRAME_INFO));
    
    gArkTsParseFrameInfoLocal = reinterpret_cast<ArkTsParseJsFrameInfoLocalFunc>(
        dlsym(gArkTsLibHandle, ARKTS_PARSE_JS_FRAME_INFO_LOCAL));
    
    gArkTsCreateLocal = reinterpret_cast<ArkTsCreateLocalFunc>(
        dlsym(gArkTsLibHandle, ARKTS_CREATE_LOCAL));
    
    gArkTsDestroyLocal = reinterpret_cast<ArkTsDestroyLocalFunc>(
        dlsym(gArkTsLibHandle, ARKTS_DESTROY_LOCAL));
    
#ifdef __OHOS__
    gArkTsTranslateJsStackInfo = reinterpret_cast<ArkTsTranslateJsStackInfoFunc>(
        dlsym(gArkTsLibHandle, ARKTS_TRANSLATE_JS_STACK_INFO));
    if (gArkTsTranslateJsStackInfo == nullptr) {
        LOG(RTLOG_INFO, "[InitArkTSUnwinder] Failed to load TranslateJSStackInfo: %s", dlerror());
    }
#endif
    
    if (gArkTsParseFrameInfoLocal == nullptr || gArkTsCreateLocal == nullptr) {
        LOG(RTLOG_INFO, "[InitArkTSUnwinder] Failed to load required functions: parseFrameInfoLocal=%p, createLocal=%p, error=%s",
            gArkTsParseFrameInfoLocal, gArkTsCreateLocal, dlerror());
        dlclose(gArkTsLibHandle);
        gArkTsLibHandle = nullptr;
        pthread_mutex_unlock(&gArkTsMutex);
        return false;
    }
    
    int createResult = gArkTsCreateLocal();
    if (createResult <= 0) {
        LOG(RTLOG_INFO, "[InitArkTSUnwinder] ark_create_local failed with result=%d", createResult);
        dlclose(gArkTsLibHandle);
        gArkTsLibHandle = nullptr;
        pthread_mutex_unlock(&gArkTsMutex);
        return false;
    }
    
    gArkTsInitialized = true;
    LOG(RTLOG_INFO, "[InitArkTSUnwinder] ArkTS unwinder initialized successfully, parseFrameInfoLocal=%p, parseFrameInfo=%p",
        gArkTsParseFrameInfoLocal, gArkTsParseFrameInfo);
    
    pthread_mutex_unlock(&gArkTsMutex);
    return true;
}

void CleanupArkTSUnwinder()
{
    pthread_mutex_lock(&gArkTsMutex);
    
    for (auto& pair : gExtractorCache) {
        if (pair.second != 0 && gArkTsDestroyExtractor != nullptr) {
            gArkTsDestroyExtractor(pair.second);
        }
    }
    gExtractorCache.clear();
    gExtractorToPath.clear();
    
    if (gArkTsDestroyLocal != nullptr) {
        gArkTsDestroyLocal();
    }
    
    if (gArkTsLibHandle != nullptr) {
        dlclose(gArkTsLibHandle);
        gArkTsLibHandle = nullptr;
    }
    
    gArkTsCreateExtractor = nullptr;
    gArkTsDestroyExtractor = nullptr;
    gArkTsParseFileInfo = nullptr;
    gArkTsParseFrameInfo = nullptr;
    gArkTsParseFrameInfoLocal = nullptr;
    gArkTsCreateLocal = nullptr;
    gArkTsDestroyLocal = nullptr;
#ifdef __OHOS__
    gArkTsTranslateJsStackInfo = nullptr;
#endif
    gArkTsInitialized = false;
    gArkTsInitAttempted = false;
    
    pthread_mutex_unlock(&gArkTsMutex);
}

void ArkTSStackMetadataHelper::ExtractInfoFromIP()
{
    LOG(RTLOG_ERROR, "[ExtractInfoFromIP] Enter");
    
    InitArkTSUnwinder();
    
#ifdef __aarch64__
    constexpr uintptr_t PAC_MASK = 0x0000007fffffffffULL;
    uintptr_t strippedIp = ip_ & PAC_MASK;
#else
    uintptr_t strippedIp = ip_;
#endif
    
    if (gArkTsParseFrameInfoLocal == nullptr) {
        LOG(RTLOG_ERROR, "[ExtractInfoFromIP] ark_parse_js_frame_info_local not available");
        Dl_info dlInfo;
        int dladdrRet = dladdr(reinterpret_cast<void*>(ip_), &dlInfo);
        if (dladdrRet) {
            isValid_ = true;
            if (dlInfo.dli_fname != nullptr) {
                fileName_ = CString(dlInfo.dli_fname);
            }
            if (dlInfo.dli_sname != nullptr) {
                methodName_ = CString(dlInfo.dli_sname);
            }
            className_ = CString("ArkTS");
            lineNumber_ = 0;
        } else {
            isValid_ = false;
        }
        return;
    }
    
    MapInfo mapInfo;
    if (!GetMapInfoForAddress(strippedIp, mapInfo)) {
        LOG(RTLOG_ERROR, "[ExtractInfoFromIP] Failed to get map info for address 0x%lx", strippedIp);
        isValid_ = false;
        return;
    }
    
    LOG(RTLOG_ERROR, "[ExtractInfoFromIP] mapBase=0x%lx, offset=0x%lx, path=%s, size=%lu",
        mapInfo.begin, mapInfo.offset, mapInfo.pathname, mapInfo.end - mapInfo.begin);
    
    ArkTSJsFrame* jsFrame = new ArkTSJsFrame();
    jsFrame->functionName[0] = '\0';
    jsFrame->packageName[0] = '\0';
    jsFrame->url[0] = '\0';
    jsFrame->line = 0;
    jsFrame->column = 0;
    
    int result = -1;
    
    if (IsHapMap(mapInfo.pathname)) {
        LOG(RTLOG_ERROR, "[ExtractInfoFromIP] Calling ark_parse_js_frame_info_local for HAP: pc=0x%lx, mapBase=0x%lx, offset=0x%lx",
            strippedIp, mapInfo.begin, mapInfo.offset);
        
        result = gArkTsParseFrameInfoLocal(strippedIp, mapInfo.begin, mapInfo.offset, jsFrame);
    } else if (IsArkCodeMap(mapInfo.pathname)) {
        uintptr_t dataSize = mapInfo.end - mapInfo.begin;
        uint8_t* dataPtr = reinterpret_cast<uint8_t*>(mapInfo.begin);
        
        uintptr_t extractorPtr = GetOrCreateExtractorForPath(mapInfo.pathname);
        if (extractorPtr == 0) {
            LOG(RTLOG_ERROR, "[ExtractInfoFromIP] Failed to get extractor for .ark path: %s", mapInfo.pathname);
            delete jsFrame;
            isValid_ = false;
            return;
        }
        
        LOG(RTLOG_ERROR, "[ExtractInfoFromIP] Calling ark_parse_js_frame_info for .ark: pc=0x%lx, mapBase=0x%lx, loadOffset=0x%lx, dataSize=%lu",
            strippedIp, mapInfo.begin, mapInfo.offset, dataSize);
        
        result = gArkTsParseFrameInfo(strippedIp, mapInfo.begin, mapInfo.offset,
                                      dataPtr, dataSize, extractorPtr, jsFrame);
    } else {
        LOG(RTLOG_ERROR, "[ExtractInfoFromIP] Not a .hap/.ark map, path=%s", mapInfo.pathname);
    }
    
    if (result > 0) {
#ifdef __OHOS__
        if (gArkTsTranslateJsStackInfo != nullptr) {
            unsigned long long arkVM = CJGetArkVM();
            if (arkVM != 0) {
                std::string url(jsFrame->url);
                std::string packageName(jsFrame->packageName);
                int line = jsFrame->line;
                int column = jsFrame->column;
                
                gArkTsTranslateJsStackInfo(arkVM, url, line, column, packageName);
                
                strncpy(jsFrame->url, url.c_str(), ARKTS_URL_MAX - 1);
                jsFrame->url[ARKTS_URL_MAX - 1] = '\0';
                jsFrame->line = line;
                jsFrame->column = column;
                strncpy(jsFrame->packageName, packageName.c_str(), ARKTS_PACKAGENAME_MAX - 1);
                jsFrame->packageName[ARKTS_PACKAGENAME_MAX - 1] = '\0';
                
                LOG(RTLOG_ERROR, "[ExtractInfoFromIP] After SourceMap translate: url=%s, line=%d, column=%d, packageName=%s",
                    jsFrame->url, jsFrame->line, jsFrame->column, jsFrame->packageName);
            }
        }
#endif
        isValid_ = true;
        LOG(RTLOG_ERROR, "[ExtractInfoFromIP] parse SUCCESS: url=%s, line=%d, func=%s, packageName=%s",
            jsFrame->url, jsFrame->line, jsFrame->functionName, jsFrame->packageName);
        if (jsFrame->url[0] != '\0') {
            fileName_ = CString(jsFrame->url);
        }
        if (jsFrame->functionName[0] != '\0') {
            methodName_ = CString(jsFrame->functionName);
        }
        if (jsFrame->packageName[0] != '\0') {
            className_ = CString(jsFrame->packageName);
        }
        lineNumber_ = static_cast<uint32_t>(jsFrame->line);
        
        delete jsFrame;
        LOG(RTLOG_ERROR, "[ExtractInfoFromIP] Exit via parse success");
        return;
    }
    
    LOG(RTLOG_ERROR, "[ExtractInfoFromIP] parse FAILED, result=%d", result);
    delete jsFrame;
    
    Dl_info dlInfo;
    int dladdrRet = dladdr(reinterpret_cast<void*>(ip_), &dlInfo);
    
    if (dladdrRet) {
        isValid_ = true;
        if (dlInfo.dli_fname != nullptr) {
            fileName_ = CString(dlInfo.dli_fname);
        }
        if (dlInfo.dli_sname != nullptr) {
            methodName_ = CString(dlInfo.dli_sname);
        }
        className_ = CString("ArkTS");
        lineNumber_ = 0;
        LOG(RTLOG_ERROR, "[ExtractInfoFromIP] Exit via dladdr success");
    } else {
        isValid_ = false;
        LOG(RTLOG_ERROR, "[ExtractInfoFromIP] Exit via dladdr failed");
    }
}

} // namespace MapleRuntime