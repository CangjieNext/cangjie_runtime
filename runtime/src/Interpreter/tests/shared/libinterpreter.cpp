#include "Interpreter/RTInterface.h"
#include "libinterpreter.h"
#include "mock.h"
#include "log.h"
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#if defined(__APPLE__) && \
    (defined(__IOS__) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__))
#include <mach-o/dyld.h>
#endif

#ifndef MOCK_IMPLEMENTATION
#define MOCK_IMPLEMENTATION Mock
#endif

MOCK_IMPLEMENTATION mockInstance;
static void* g_symbol_lookup_handle = nullptr;

#if defined(__APPLE__) && \
    (defined(__IOS__) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__))
static bool HasCbcExtension(const char* fileName)
{
    const char* extension = strrchr(fileName, '.');
    return extension != nullptr && strcmp(extension, ".cbc") == 0;
}

static bool EnsureDirectory(const std::string& path)
{
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
        return true;
    }
    log("[Interpreter] Failed to create directory '%s': errno=%d (%s)", path.c_str(), errno, strerror(errno));
    return false;
}

static bool CopyFile(const std::string& sourcePath, const std::string& destinationPath)
{
    FILE* source = fopen(sourcePath.c_str(), "rb");
    if (source == nullptr) {
        log("[Interpreter] Failed to open bundled CBC '%s': errno=%d (%s)",
            sourcePath.c_str(), errno, strerror(errno));
        return false;
    }

    FILE* destination = fopen(destinationPath.c_str(), "wb");
    if (destination == nullptr) {
        log("[Interpreter] Failed to create CBC '%s': errno=%d (%s)",
            destinationPath.c_str(), errno, strerror(errno));
        fclose(source);
        return false;
    }

    bool success = true;
    char buffer[16 * 1024];
    size_t bytesRead = 0;
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, bytesRead, destination) != bytesRead) {
            log("[Interpreter] Failed to write CBC '%s': errno=%d (%s)",
                destinationPath.c_str(), errno, strerror(errno));
            success = false;
            break;
        }
    }
    if (ferror(source) != 0) {
        log("[Interpreter] Failed to read bundled CBC '%s': errno=%d (%s)",
            sourcePath.c_str(), errno, strerror(errno));
        success = false;
    }

    if (fclose(destination) != 0) {
        success = false;
    }
    fclose(source);
    return success;
}

static std::string GetExecutableDirectory()
{
    char initialBuffer[1];
    uint32_t pathSize = sizeof(initialBuffer);
    if (_NSGetExecutablePath(initialBuffer, &pathSize) == 0) {
        return {};
    }

    std::vector<char> pathBuffer(pathSize);
    if (_NSGetExecutablePath(pathBuffer.data(), &pathSize) != 0) {
        return {};
    }

    std::string executablePath(pathBuffer.data());
    size_t separator = executablePath.find_last_of('/');
    return separator == std::string::npos ? std::string() : executablePath.substr(0, separator);
}

static void PrepareBundledCbcFiles(const char* interpreterRoot)
{
    const std::string executableDirectory = GetExecutableDirectory();
    if (executableDirectory.empty()) {
        log("[Interpreter] Failed to resolve application bundle path");
        return;
    }

    const std::string sourceDirectory = executableDirectory + "/cbc";
    DIR* sourceDir = opendir(sourceDirectory.c_str());
    if (sourceDir == nullptr) {
        log("[Interpreter] Bundled CBC directory '%s' is unavailable: errno=%d (%s)",
            sourceDirectory.c_str(), errno, strerror(errno));
        return;
    }

    const std::string rootDirectory(interpreterRoot);
    const std::string destinationDirectory = rootDirectory + "/cbc";
    if (!EnsureDirectory(rootDirectory) || !EnsureDirectory(destinationDirectory)) {
        closedir(sourceDir);
        return;
    }

    int copiedFiles = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(sourceDir)) != nullptr) {
        if (!HasCbcExtension(entry->d_name)) {
            continue;
        }

        const std::string sourcePath = sourceDirectory + "/" + entry->d_name;
        struct stat statBuffer {};
        if (stat(sourcePath.c_str(), &statBuffer) != 0 || !S_ISREG(statBuffer.st_mode)) {
            continue;
        }

        const std::string destinationPath = destinationDirectory + "/" + entry->d_name;
        if (CopyFile(sourcePath, destinationPath)) {
            ++copiedFiles;
            log("[Interpreter] Prepared bundled CBC: %s", destinationPath.c_str());
        }
    }
    closedir(sourceDir);
    log("[Interpreter] Prepared %d bundled CBC file(s) in '%s'", copiedFiles, destinationDirectory.c_str());
}
#endif

#if defined(__APPLE__) && \
    (defined(__IOS__) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__))
extern "C" __attribute__((weak)) void landingPad() {}
#endif

#define C2I_STUB_FRAME_SIZE 64
#define INTERPRETER_FRAMES_SIZE 1024

extern "C" {
struct VirtFuncContainer virtualFuncs;
}

extern "C" {
    DYN_GetCurrentCatchFunctionPcFn g_GetCurrentCatchFunctionPCPtr = nullptr;
    DYN_ThrowExceptionFn g_ThrowExceptionPtr = nullptr;
    DYN_GetThreadLocalDataFn g_GetThreadLocalDataPtr = nullptr;
    DYN_StackGrowStubFn g_CJ_MCC_StackGrowStubPtr = nullptr;
    DYN_GetPendingExceptionFn g_getPendingExceptionPtr = nullptr;
    size_t g_InterpreterStackReserveSize = 0;
}

Mock::Mock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf): cjNativeInterface(*cjnative_interf) {
    interpreter_interf->version = INT_INTERPRETER_INTERFACE_VERSION;
    interpreter_interf->cjThreadSpecificDataSize = 16;
    g_InterpreterStackReserveSize = INTERPRETER_FRAMES_SIZE;

    interpreter_interf->c2iStubStartAddr = (uintptr_t) C2I_Stub;
    interpreter_interf->c2iStubEndAddr = (uintptr_t) C2I_Stub_End;

    interpreter_interf->i2iAdapterStartAddr = (uintptr_t) I2I_Stub;
    interpreter_interf->i2iAdapterEndAddr = (uintptr_t) I2I_Stub_End;

    interpreter_interf->interpreterPrologueStartAddr = 0; // mocks does not have interpreter prologue
    interpreter_interf->interpreterPrologueEndAddr = 0;

    interpreter_interf->landingPad = &LandingPadWrapper;

    interpreter_interf->iterateFramesWithState = (INT_IterateFramesWithStateFn)+[]() {
        log("[Mock] dummy iterateFramesWithState called!"); return 0;
    };
    interpreter_interf->visitGlobalRoots = (INT_VisitGlobalRootsFn)+[]() {
        log("[Mock] dummy visitGlobalRoots called!"); return 0;
    };
    interpreter_interf->visitFrameRootsExpansion = (INT_VisitFrameRootsExpansionFn)+[]() {
        log("[Mock] dummy visitFrameRootsExpansion called!"); return 0;
    };
    interpreter_interf->visitFrameRootsMarking = (INT_VisitFrameRootsMarkingFn)+[]() {
        log("[Mock] dummy visitFrameRootsMarking called!"); return 0;
    };
    interpreter_interf->visitFrameRootsAdjusting = (INT_VisitFrameRootsAdjustingFn)+[]() {
        log("[Mock] dummy visitFrameRootsAdjusting called!"); return 0;
    };
    interpreter_interf->cjThreadOnStart = (INT_CJThreadOnStartFn)+[]() {
        log("[Mock] dummy cjThreadOnStart called!"); return 0;
    };
    interpreter_interf->cjThreadOnDestroy = (INT_CJThreadOnDestroyFn)+[]() {
        log("[Mock] dummy cjThreadOnDestroy called!"); return 0;
    };
    interpreter_interf->frameInfoProvider = (INT_FrameInfoProviderFn)+[]() {
        log("[Mock] dummy frameInfoProvider called!"); return 0;
    };

    g_GetCurrentCatchFunctionPCPtr = cjnative_interf->getCurrentCatchFunctionPc;
    g_ThrowExceptionPtr = cjnative_interf->throwException;
    g_GetThreadLocalDataPtr = cjnative_interf->getThreadLocalData;

    g_CJ_MCC_StackGrowStubPtr = cjnative_interf->stackGrowStub;
    g_getPendingExceptionPtr = cjnative_interf->getPendingException;
    g_symbol_lookup_handle = cjnative_interf->appLibHandle;
}

// Public library API

PUBLIC int interpreter_bridge_init(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf, int interpreter_args_count, const char* const* interpreter_args) {
        initLogs();
        log("[Interpreter] Start interfaces exchange");
        if (cjnative_interf == nullptr) {
            log("[Interpreter] CJNative interface is null");
            return 1;
        }
        if (cjnative_interf->version != DYN_CJNATIVE_INTERFACE_VERSION) {
            log("[Interpreter] Unsupported CJNative interface version: %lld, expected: %d",
                static_cast<long long>(cjnative_interf->version), DYN_CJNATIVE_INTERFACE_VERSION);
            return 1;
        }
        log("[Interpreter] Interpreter arguments count: %d", interpreter_args_count);
        for (int i = 0; i < interpreter_args_count; ++i) {
            log("[Interpreter] Interpreter argument #%d: %s", i, interpreter_args[i]);
        }
        if (interpreter_args_count > 0 && interpreter_args != nullptr && interpreter_args[0] != nullptr && interpreter_args[0][0] == '/') {
            const char* dirPath = interpreter_args[0];
            log("[Interpreter] First argument looks like an absolute path: %s", dirPath);
#if defined(__APPLE__) && \
    (defined(__IOS__) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__))
            PrepareBundledCbcFiles(dirPath);
#endif
            DIR* dir = opendir(dirPath);
            if (dir == nullptr) {
                log("[Interpreter] Failed to open directory '%s': errno=%d (%s)", dirPath, errno, strerror(errno));
            } else {
                log("[Interpreter] Listing directory contents for '%s':", dirPath);
                struct dirent* entry = nullptr;
                while ((entry = readdir(dir)) != nullptr) {
                    log("[Interpreter]   %s", entry->d_name);
                }
                closedir(dir);

                const std::string cbcPath = std::string(dirPath) + "/cbc";
                DIR* cbcDir = opendir(cbcPath.c_str());
                if (cbcDir == nullptr) {
                    if (errno == ENOENT) {
                        log("[Interpreter] Directory '%s' does not exist, skipping", cbcPath.c_str());
                    } else {
                        log("[Interpreter] Failed to open directory '%s': errno=%d (%s)", cbcPath.c_str(), errno, strerror(errno));
                    }
                } else {
                    log("[Interpreter] Listing files in '%s' with size:", cbcPath.c_str());
                    struct dirent* cbcEntry = nullptr;
                    while ((cbcEntry = readdir(cbcDir)) != nullptr) {
                        if (strcmp(cbcEntry->d_name, ".") == 0 || strcmp(cbcEntry->d_name, "..") == 0) {
                            continue;
                        }

                        const std::string fullPath = cbcPath + "/" + cbcEntry->d_name;
                        struct stat statBuffer {};
                        if (stat(fullPath.c_str(), &statBuffer) != 0) {
                            log("[Interpreter]   %s: failed to read stat, errno=%d (%s)",
                                cbcEntry->d_name, errno, strerror(errno));
                            continue;
                        }
                        if (S_ISREG(statBuffer.st_mode)) {
                            log("[Interpreter]   %s: %lld bytes",
                                cbcEntry->d_name, static_cast<long long>(statBuffer.st_size));
                        }
                    }
                    closedir(cbcDir);
                }
            }
        }

        mockInstance = MOCK_IMPLEMENTATION(interpreter_interf, cjnative_interf);

        log("[Interpreter] End of interfaces exchange");
        return 0;
}

PUBLIC void* getC2IFunc(int idx) {
        uint8_t* tStartAddr = &trampolinesStart;
        uint8_t* trampolineAddr = tStartAddr + (idx * TRAMPOLINE_SIZE);
        return trampolineAddr;
}

// Public library API end

/**
 * Tries to find a symbol in the provided handle scope or the global scope (RTLD_DEFAULT).
 * Returns the address if found, or NULL if not.
 */
void* get_symbol_address(const char* symbol_name) {
    // Clear any existing errors
    dlerror();

    void* lookup_handle = g_symbol_lookup_handle != nullptr ? g_symbol_lookup_handle : RTLD_DEFAULT;
    void* addr = dlsym(lookup_handle, symbol_name);
    const char* err = dlerror();

    if (err != nullptr) {
        log("[Mock] error during symbol lookup: %s\n", symbol_name);
        log("[Mock] error: %s\n", err);
        return NULL;
    }

    return addr;
}
