#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>

#define check(cond, msg) \
    do {                 \
        if (!(cond)) {   \
            printf(msg);    \
            exit(1);     \
        }                \
    } while (0)

#endif