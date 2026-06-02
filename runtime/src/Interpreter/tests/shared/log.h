#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && \
    (defined(__IOS__) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__) || \
     (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE))
#define INTERPRETER_TEST_IOS_LOG 1
#else
#define INTERPRETER_TEST_IOS_LOG 0
#endif

#if defined(__ANDROID__)
#include <android/log.h>
// Redefine printf to map to the Android logcat info channel
#define printf(...) __android_log_print(ANDROID_LOG_INFO, "Mock", __VA_ARGS__)
#elif INTERPRETER_TEST_IOS_LOG
#include <os/log.h>
#include <stdarg.h>

static inline int interpreterTestLogPrint(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static inline int interpreterTestLogPrint(const char* fmt, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    int result = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (result < 0) {
        os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "%{public}s",
                         "[InterpreterTests] failed to format log message");
        return result;
    }

    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_INFO, "%{public}s", buffer);
    return result;
}

#define printf(...) interpreterTestLogPrint(__VA_ARGS__)
#endif

#if !defined(__ANDROID__) && !INTERPRETER_TEST_IOS_LOG
extern FILE* logFile;
#endif

void initLogs();

#if defined(__ANDROID__)
#define log(fmt, ...) \
    __android_log_print(ANDROID_LOG_INFO, "InterpreterTests", fmt, ##__VA_ARGS__)
#elif INTERPRETER_TEST_IOS_LOG
#define log(fmt, ...) \
    interpreterTestLogPrint(fmt "\n", ##__VA_ARGS__)
#else
#define log(fmt, ...) \
    fprintf(logFile, fmt "\n", ##__VA_ARGS__)
#endif

#endif
