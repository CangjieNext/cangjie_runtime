#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#if defined(__ANDROID__)
#include <android/log.h>
// Redefine printf to map to the Android logcat info channel
#define printf(...) __android_log_print(ANDROID_LOG_INFO, "Mock", __VA_ARGS__)
#endif

#if !defined(__ANDROID__)
extern FILE* logFile;
#endif

void initLogs();

#if defined(__ANDROID__)
#define log(fmt, ...) \
    __android_log_print(ANDROID_LOG_INFO, "InterpreterTests", fmt, ##__VA_ARGS__)
#else
#define log(fmt, ...) \
    fprintf(logFile, fmt "\n", ##__VA_ARGS__)
#endif

#endif
