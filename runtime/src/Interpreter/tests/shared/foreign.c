#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && \
    (defined(__IOS__) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__) || \
     (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE))
#include "log.h"
#else
#include <stdio.h>
#endif
#include <unistd.h>
#include <stdint.h>
#include <dlfcn.h>

#if defined(__APPLE__) && \
    (defined(__IOS__) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__) || \
     (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE))
#define INTERPRETER_MOCK_LIB_NAME "libcbcengine.dylib"
#else
#define INTERPRETER_MOCK_LIB_NAME "libcbcengine.so"
#endif

#ifdef __cplusplus
extern "C" {
#endif

void foreignCanBeCalled() {
    printf("[foreign] foreignCanBeCalled: OK\n");
}

void longRunningForeignCall() {
    int sleepSeconds = 5;
    printf("[foreign] longRunningForeignCall: started, sleeping for %d seconds...\n", sleepSeconds);
    sleep(sleepSeconds);
    printf("[foreign] longRunningForeignCall: woke up, finished\n");
}

void restoreInterpreterStackReserveSize() {
    void* handle = dlopen(INTERPRETER_MOCK_LIB_NAME, RTLD_LAZY);
    if (handle == NULL) {
        printf("[foreign] restoreInterpreterStackReserveSize: %s is unavailable: %s\n",
               INTERPRETER_MOCK_LIB_NAME, dlerror());
        return;
    }

    typedef void (*restore_func_t)(void);
    restore_func_t restore = (restore_func_t)dlsym(handle, "restoreMockInterpreterStackReserveSize");
    if (restore == NULL) {
        printf("[foreign] restoreInterpreterStackReserveSize: restore symbol is unavailable: %s\n", dlerror());
        return;
    }

    restore();
}


typedef void (*callback)(int);
static callback my_stored_callback = NULL;

void set_callback(callback cb) {
    my_stored_callback = cb;
    printf("[foreign] set_callback: callback has been set successfully\n");
}

void call_callback(int value) {
    if (my_stored_callback) {
        printf("[foreign] call_callback: calling callback with arg %d\n", value);
        my_stored_callback(value);
    } else {
        printf("[foreign] ERROR: no callback has been set\n");
    }
}

#ifdef __cplusplus
}
#endif
