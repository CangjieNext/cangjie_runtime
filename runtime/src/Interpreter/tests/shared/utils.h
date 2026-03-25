#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>

/* Interpreter tests: arm64 CI/machines are much slower; shrink heavy loops by 100x. */
#if defined(__aarch64__) || defined(__AARCH64EL__) || defined(_M_ARM64)
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