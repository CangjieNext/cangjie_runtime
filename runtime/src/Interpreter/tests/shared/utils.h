#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>

/* Interpreter tests: mobile targets and arm64 CI/machines are much slower; shrink heavy loops. */
#define CJ_INTERPRETER_TEST_IOS_SIMULATOR_PAYLOAD_DIV 3000
#define CJ_INTERPRETER_TEST_MOBILE_PAYLOAD_DIV 300

#if defined(__APPLE__) && (defined(__APPLE_EMBEDDED_SIMULATOR__) || (defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR))
#define CJ_INTERPRETER_TEST_PAYLOAD_DIV CJ_INTERPRETER_TEST_IOS_SIMULATOR_PAYLOAD_DIV
#elif defined(__ANDROID__) || defined(__IOS__) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
#define CJ_INTERPRETER_TEST_PAYLOAD_DIV CJ_INTERPRETER_TEST_MOBILE_PAYLOAD_DIV
#elif defined(__aarch64__) || defined(__AARCH64EL__) || defined(_M_ARM64)
#define CJ_INTERPRETER_TEST_PAYLOAD_DIV 10
#else
#define CJ_INTERPRETER_TEST_PAYLOAD_DIV 1
#endif
#define CJ_INTERPRETER_TEST_ITER(n) ((n) / CJ_INTERPRETER_TEST_PAYLOAD_DIV)

#define check(cond, msg, ...)  \
    do {                       \
        if (!(cond)) {         \
            printf(msg, ##__VA_ARGS__);  \
            exit(1);           \
        }                      \
    } while (0)

#endif
